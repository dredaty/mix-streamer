/**
 * Binary frame format sent by the plugin on ws://127.0.0.1:8765/ingest
 * and forwarded unchanged to every listener on /listen.
 *
 *  offset size  field
 *  0      4     magic "MXS1"
 *  4      4     sampleRate      uint32 LE
 *  8      1     bitDepth        uint8  (16 | 24)
 *  9      1     channels        uint8  (2)
 *  10     2     flags           uint16 LE (reserved, 0)
 *  12     4     sequence        uint32 LE (frame counter, wraps)
 *  16     ...   PCM, interleaved, signed little-endian
 */
export const HEADER_SIZE = 16;
export const MAGIC = 0x3153584d; // "MXS1" read as uint32 LE

export interface StreamFormat {
  sampleRate: number;
  bitDepth: number;
  channels: number;
}

export function parseHeader(buf: Buffer): (StreamFormat & { sequence: number }) | null {
  if (buf.length < HEADER_SIZE) return null;
  if (buf.readUInt32LE(0) !== MAGIC) return null;
  const sampleRate = buf.readUInt32LE(4);
  const bitDepth = buf.readUInt8(8);
  const channels = buf.readUInt8(9);
  if (sampleRate < 8000 || sampleRate > 384000) return null;
  if (bitDepth !== 16 && bitDepth !== 24) return null;
  if (channels < 1 || channels > 2) return null;
  const frameBytes = (bitDepth / 8) * channels;
  if ((buf.length - HEADER_SIZE) % frameBytes !== 0) return null;
  return { sampleRate, bitDepth, channels, sequence: buf.readUInt32LE(12) };
}
