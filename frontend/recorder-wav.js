async function recordAndEncodeWav(stream, seconds = 5) {
  const ctx = new (window.AudioContext || window.webkitAudioContext)({ sampleRate: 44100 });
  const source = ctx.createMediaStreamSource(stream);

  const bufferSize = 4096;
  const processor = ctx.createScriptProcessor(bufferSize, 1, 1);
  const samples = [];

  processor.onaudioprocess = e => {
    const ch = e.inputBuffer.getChannelData(0);
    samples.push(new Float32Array(ch));
  };

  source.connect(processor);
  processor.connect(ctx.destination);

  await new Promise(res => setTimeout(res, seconds * 1000));

  processor.disconnect();
  source.disconnect();
  ctx.close();

  let length = samples.reduce((a, b) => a + b.length, 0);
  let pcm = new Float32Array(length);
  let off = 0;
  for (const chunk of samples) {
    pcm.set(chunk, off);
    off += chunk.length;
  }

  const wavBytes = encodeWav(pcm, 44100);
  return new Blob([wavBytes], { type: "audio/wav" });
}

function encodeWav(samples, sampleRate) {
  const bytesPerSample = 2;
  const blockAlign = 1 * bytesPerSample;
  const byteRate = sampleRate * blockAlign;
  const dataSize = samples.length * bytesPerSample;
  const buffer = new ArrayBuffer(44 + dataSize);
  const view = new DataView(buffer);

  let offset = 0;
  writeString(view, offset, 'RIFF'); offset += 4;
  view.setUint32(offset, 36 + dataSize, true); offset += 4;
  writeString(view, offset, 'WAVE'); offset += 4;
  writeString(view, offset, 'fmt '); offset += 4;
  view.setUint32(offset, 16, true); offset += 4;
  view.setUint16(offset, 1, true); offset += 2;
  view.setUint16(offset, 1, true); offset += 2;
  view.setUint32(offset, sampleRate, true); offset += 4;
  view.setUint32(offset, byteRate, true); offset += 4;
  view.setUint16(offset, blockAlign, true); offset += 2;
  view.setUint16(offset, 16, true); offset += 2;
  writeString(view, offset, 'data'); offset += 4;
  view.setUint32(offset, dataSize, true); offset += 4;

  let pos = 44;
  for (let i = 0; i < samples.length; i++, pos += 2) {
    let s = Math.max(-1, Math.min(1, samples[i]));
    view.setInt16(pos, s < 0 ? s * 0x8000 : s * 0x7FFF, true);
  }

  return buffer;
}

function writeString(view, offset, str) {
  for (let i = 0; i < str.length; i++) {
    view.setUint8(offset + i, str.charCodeAt(i));
  }
}
