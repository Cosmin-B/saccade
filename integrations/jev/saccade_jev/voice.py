"""Optional local speech control for long-running game sessions."""
import queue
import re
import threading

from .control import JevError


def parse_voice_command(transcript):
    if not isinstance(transcript, str) or len(transcript) > 256:
        return None
    words = re.sub(r'[^a-z ]', ' ', transcript.lower())
    words = ' '.join(words.split())
    return {'saccade start healing': 'resume',
            'saccade resume healing': 'resume',
            'saccade pause healing': 'pause',
            'saccade stop healing': 'stop'}.get(words)


class LocalVoiceMonitor:
    """Four-second microphone clips; model output is an exact, closed command set."""
    def __init__(self, *, device='cpu'):
        if device not in ('cpu', 'mps'):
            raise JevError('Voice device must be cpu or mps')
        self.device = device
        self.commands = queue.Queue(maxsize=8)
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, name='saccade-voice', daemon=True)

    def start(self):
        self._thread.start()

    def stop(self):
        self._stop.set()
        self._thread.join(timeout=1)

    def poll(self):
        try:
            item = self.commands.get_nowait()
        except queue.Empty:
            return None
        if isinstance(item, Exception):
            raise JevError('Local voice capture or transcription failed') from item
        return item

    def _run(self):
        try:
            import moondream as md
            import sounddevice as sd
            with md.photon('moondream/parakeet-redux', device=self.device) as speech:
                with sd.InputStream(samplerate=16000, channels=1, dtype='float32') as microphone:
                    while not self._stop.is_set():
                        samples, overflow = microphone.read(16000 * 4)
                        if overflow:
                            continue
                        result = speech.transcribe(audio=samples[:, 0].copy(), sample_rate=16000)
                        command = parse_voice_command(result.get('text'))
                        if command:
                            try:
                                self.commands.put_nowait(command)
                            except queue.Full:
                                pass
        except Exception as error:
            try:
                self.commands.put_nowait(error)
            except queue.Full:
                pass
