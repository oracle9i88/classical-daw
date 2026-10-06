import { renderAudio } from './audio-engine.mjs';

globalThis.onmessage = ({ data }) => {
  try {
    const result = renderAudio(data.project, { ...data.range, sampleRate: data.sampleRate ?? 48000 },
      value => globalThis.postMessage({ type: 'progress', value }));
    globalThis.postMessage({ type: 'result', ...result }, [result.left.buffer, result.right.buffer]);
  } catch (error) {
    globalThis.postMessage({ type: 'error', message: error instanceof Error ? error.message : String(error) });
  }
};
