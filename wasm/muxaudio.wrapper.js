/**
 * MuxAudio WebAssembly Wrapper
 * Minimal JavaScript wrapper around muxaudio WASM module
 */

// Codec name to type mapping
const CODEC_TYPES = {
  pcm: 0,
  opus: 1,
  vorbis: 2,
  flac: 3,
  mp3: 4,
  aac: 5
};

// Stream types
const STREAM_AUDIO = 0;
const STREAM_SIDE_CHANNEL = 1;

export class MuxEncoder {
  constructor(module, codec, sampleRate, channels, params = {}) {
    this.module = module;
    this.sampleRate = sampleRate;
    this.channels = channels;

    // Resolve codec name to type
    if (typeof codec === 'string') {
      this.codecType = CODEC_TYPES[codec.toLowerCase()];
      if (this.codecType === undefined) {
        throw new Error(`Unknown codec: ${codec}`);
      }
    } else {
      this.codecType = codec;
    }

    // Bind C functions
    this._new = module.cwrap('mux_encoder_new', 'number',
      ['number', 'number', 'number', 'number', 'number']);
    this._destroy = module.cwrap('mux_encoder_destroy', null, ['number']);
    this._encode = module.cwrap('mux_encoder_encode', 'number',
      ['number', 'number', 'number', 'number', 'number']);
    this._read = module.cwrap('mux_encoder_read', 'number',
      ['number', 'number', 'number', 'number']);
    this._finalize = module.cwrap('mux_encoder_finalize', 'number', ['number']);

    // Create encoder with parameters
    const { paramsPtr, numParams } = this._createParams(params);

    this.encoder = this._new(
      this.codecType,
      sampleRate,
      channels,
      paramsPtr,
      numParams
    );

    this._freeParams(paramsPtr, numParams);

    if (!this.encoder) {
      throw new Error(`Failed to create ${codec} encoder`);
    }
  }

  _createParams(params) {
    const { _malloc, _free, stringToUTF8, lengthBytesUTF8, setValue } = this.module;
    const paramKeys = Object.keys(params);
    const numParams = paramKeys.length;

    if (numParams === 0) {
      return { paramsPtr: 0, numParams: 0 };
    }

    // Allocate array of mux_param structs
    // struct mux_param { const char *name; union { int i; float f; ... } value; }
    // Size: 8 bytes (4 for pointer + 4 for union)
    const structSize = 8;
    const paramsPtr = _malloc(structSize * numParams);

    for (let i = 0; i < numParams; i++) {
      const key = paramKeys[i];
      const value = params[key];
      const offset = paramsPtr + i * structSize;

      // Allocate and set name string
      const nameLen = lengthBytesUTF8(key) + 1;
      const namePtr = _malloc(nameLen);
      stringToUTF8(key, namePtr, nameLen);
      setValue(offset, namePtr, 'i32'); // name pointer

      // Set value (union - we'll use int for simplicity)
      if (typeof value === 'number') {
        setValue(offset + 4, value, 'i32'); // value.i
      } else {
        throw new Error(`Unsupported parameter type for ${key}`);
      }
    }

    return { paramsPtr, numParams };
  }

  _freeParams(paramsPtr, numParams) {
    if (paramsPtr === 0) return;

    const { _free, getValue } = this.module;
    const structSize = 8;

    for (let i = 0; i < numParams; i++) {
      const offset = paramsPtr + i * structSize;
      const namePtr = getValue(offset, 'i32');
      _free(namePtr);
    }
    _free(paramsPtr);
  }

  encode(data, streamType = STREAM_AUDIO) {
    const { _malloc, _free } = this.module;

    // Determine if input is audio (Int16Array) or side channel (Uint8Array)
    let inputPtr, numBytes, heap;

    if (data instanceof Int16Array || (data.buffer && data.BYTES_PER_ELEMENT === 2)) {
      // Audio data
      numBytes = data.length * 2;
      inputPtr = _malloc(numBytes);
      this.module.HEAP16.set(data, inputPtr / 2);
    } else if (data instanceof Uint8Array || ArrayBuffer.isView(data)) {
      // Side channel data
      numBytes = data.length;
      inputPtr = _malloc(numBytes);
      this.module.HEAPU8.set(data, inputPtr);
    } else {
      throw new Error('Data must be Int16Array (audio) or Uint8Array (side channel)');
    }

    const consumedPtr = _malloc(8);

    const result = this._encode(
      this.encoder,
      inputPtr,
      numBytes,
      consumedPtr,
      streamType
    );

    _free(inputPtr);
    _free(consumedPtr);

    if (result !== 0) {
      throw new Error(`Encode failed: error code ${result}`);
    }
  }

  read() {
    const { _malloc, _free, HEAPU8, getValue } = this.module;
    const maxSize = 4 * 1024 * 1024; // 4MB buffer
    const outputPtr = _malloc(maxSize);
    const writtenPtr = _malloc(8);

    const result = this._read(this.encoder, outputPtr, maxSize, writtenPtr);

    if (result === 0) {
      const written = getValue(writtenPtr, 'i64');
      const output = HEAPU8.slice(outputPtr, outputPtr + Number(written));
      _free(outputPtr);
      _free(writtenPtr);
      return output;
    }

    _free(outputPtr);
    _free(writtenPtr);

    if (result === -4) { // MUX_ERROR_AGAIN - no data available
      return new Uint8Array(0);
    }

    throw new Error(`Read failed: error code ${result}`);
  }

  finalize() {
    const result = this._finalize(this.encoder);
    if (result !== 0) {
      throw new Error(`Finalize failed: error code ${result}`);
    }

    // Read all remaining data
    const chunks = [];
    let chunk;
    while ((chunk = this.read()).length > 0) {
      chunks.push(chunk);
    }

    if (chunks.length === 0) {
      return new Uint8Array(0);
    }

    // Concatenate chunks
    const totalLength = chunks.reduce((sum, c) => sum + c.length, 0);
    const result_data = new Uint8Array(totalLength);
    let offset = 0;
    for (const chunk of chunks) {
      result_data.set(chunk, offset);
      offset += chunk.length;
    }

    return result_data;
  }

  destroy() {
    if (this.encoder) {
      this._destroy(this.encoder);
      this.encoder = null;
    }
  }
}

export class MuxDecoder {
  constructor(module, codec, params = {}) {
    this.module = module;

    // Resolve codec name to type
    if (typeof codec === 'string') {
      this.codecType = CODEC_TYPES[codec.toLowerCase()];
      if (this.codecType === undefined) {
        throw new Error(`Unknown codec: ${codec}`);
      }
    } else {
      this.codecType = codec;
    }

    // Bind C functions
    this._new = module.cwrap('mux_decoder_new', 'number',
      ['number', 'number', 'number']);
    this._destroy = module.cwrap('mux_decoder_destroy', null, ['number']);
    this._decode = module.cwrap('mux_decoder_decode', 'number',
      ['number', 'number', 'number', 'number']);
    this._read = module.cwrap('mux_decoder_read', 'number',
      ['number', 'number', 'number', 'number', 'number']);
    this._finalize = module.cwrap('mux_decoder_finalize', 'number', ['number']);

    // Create decoder (params usually not needed for decoders)
    this.decoder = this._new(this.codecType, 0, 0);

    if (!this.decoder) {
      throw new Error(`Failed to create ${codec} decoder`);
    }
  }

  decode(data) {
    const { _malloc, _free, HEAPU8 } = this.module;

    const inputPtr = _malloc(data.length);
    HEAPU8.set(data, inputPtr);

    const consumedPtr = _malloc(8);

    const result = this._decode(
      this.decoder,
      inputPtr,
      data.length,
      consumedPtr
    );

    _free(inputPtr);
    _free(consumedPtr);

    if (result !== 0 && result !== -6) { // MUX_ERROR_EOF (-6) is ok
      throw new Error(`Decode failed: error code ${result}`);
    }
  }

  read() {
    const { _malloc, _free, HEAP16, HEAPU8, getValue } = this.module;
    const maxSize = 4 * 1024 * 1024; // 4MB buffer
    const outputPtr = _malloc(maxSize);
    const writtenPtr = _malloc(8);
    const streamTypePtr = _malloc(4);

    const result = this._read(
      this.decoder,
      outputPtr,
      maxSize,
      writtenPtr,
      streamTypePtr
    );

    if (result === 0) {
      const written = getValue(writtenPtr, 'i64');
      const streamType = getValue(streamTypePtr, 'i32');

      let data;
      if (streamType === STREAM_AUDIO) {
        // Audio data - return as Int16Array
        const samples = Number(written) / 2;
        data = new Int16Array(this.module.HEAP16.buffer, outputPtr, samples).slice();
      } else {
        // Side channel data - return as Uint8Array
        data = HEAPU8.slice(outputPtr, outputPtr + Number(written));
      }

      _free(outputPtr);
      _free(writtenPtr);
      _free(streamTypePtr);

      return { data, streamType };
    }

    _free(outputPtr);
    _free(writtenPtr);
    _free(streamTypePtr);

    if (result === -4) { // MUX_ERROR_AGAIN
      return { data: new Uint8Array(0), streamType: STREAM_AUDIO };
    }

    throw new Error(`Read failed: error code ${result}`);
  }

  finalize() {
    const result = this._finalize(this.decoder);
    if (result !== 0 && result !== -6) { // MUX_ERROR_EOF is ok
      throw new Error(`Finalize failed: error code ${result}`);
    }

    // Read all remaining data
    const outputs = [];
    let output;
    while ((output = this.read()).data.length > 0) {
      outputs.push(output);
    }

    return outputs;
  }

  destroy() {
    if (this.decoder) {
      this._destroy(this.decoder);
      this.decoder = null;
    }
  }
}

// Main factory function
export async function createMuxAudio(wasmPath) {
  // Load the WASM module
  let createModule;

  if (wasmPath) {
    // Custom WASM path
    createModule = await import(wasmPath);
  } else {
    // Try to load from same directory
    const Module = await createMuxAudioWasm();

    return {
      Module,
      Encoder: (codec, sr, ch, params) => new MuxEncoder(Module, codec, sr, ch, params),
      Decoder: (codec, params) => new MuxDecoder(Module, codec, params),

      // Codec constants
      CODEC: CODEC_TYPES,
      STREAM_AUDIO,
      STREAM_SIDE_CHANNEL
    };
  }
}

// Browser global export
if (typeof window !== 'undefined') {
  window.createMuxAudio = createMuxAudio;
  window.MuxEncoder = MuxEncoder;
  window.MuxDecoder = MuxDecoder;
}
