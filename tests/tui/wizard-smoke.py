# Run on POSIX against a built launcher with setup/ and the SQLite plugin beside it.
# No installation is applied; the synthetic client is used only for plan validation.
import os,pty,fcntl,termios,struct,select,time,signal,sys,tempfile
from pathlib import Path
exe=str(Path(sys.argv[1]).resolve())
scratch=tempfile.TemporaryDirectory(prefix='lonelyice-wizard-')
base=Path(scratch.name)
client=base/'client';(client/'Data').mkdir(parents=True,exist_ok=True)
(client/'Wow.exe').touch();(client/'Data/common.MPQ').touch()
profile=base/'server.yaml';profile.write_text(f'format: 1\nclient:\n  path: {client}\nserver:\n  root: {base}/server\n')
pid,fd=pty.fork()
if pid==0:
 os.environ['TERM']='xterm-256color';os.environ['LONELYICE_LANG']='en'
 os.execv(exe,[exe,'--tui','--settings',str(profile)])
fcntl.ioctl(fd,termios.TIOCSWINSZ,struct.pack('HHHH',36,120,0,0))
output=bytearray()
def drain(seconds=.3):
 end=time.monotonic()+seconds
 while time.monotonic()<end:
  if select.select([fd],[],[],.05)[0]:
   try: output.extend(os.read(fd,65536))
   except OSError:return

def send(key):
 os.write(fd,key);drain()
def expect(value):
 text=output.decode(errors='replace');assert value in text,(value,text[-3500:]);output.clear()
try:
 drain(1);expect('Installation wizard')
 # The first path is focused automatically. Tab reaches client, then Next.
 send(b'\t');send(b'\t');send(b'\r');expect('step 2 of 6')
 # Next retains keyboard focus between steps.
 send(b'\r');expect('step 3 of 6')
 send(b'\r');drain(2);expect('step 4 of 6')
 # Back from the plan goes to components; fields and navigation remain reachable.
 send(b'\x1b[Z');send(b'\r');expect('step 3 of 6')
 send(b'\t');send(b'\r');drain(1);expect('step 4 of 6')
 # Applying an unconfirmed plan must be rejected without starting the installer.
 send(b'\x1b[Z');send(b'\x1b[Z');send(b'\r');expect('Review this plan and confirm it before applying.')
 assert not (base/'local.yaml').exists(), 'Review must not commit local overrides'
 assert profile.read_text().startswith('format: 1\nclient:'), 'Review must not change the profile'
 send(b'\x1b[21~');drain(.5)
 child,status=os.waitpid(pid,os.WNOHANG)
 assert child and os.waitstatus_to_exitcode(status)==0, 'F10 should close the clean wizard'
 pid=None
 print('PASS: first-run wizard, keyboard navigation, storage check, plan, Back, confirmation guard and clean exit')
finally:

 if pid:
  try:os.kill(pid,signal.SIGTERM)
  except ProcessLookupError:pass
  os.waitpid(pid,0)
 scratch.cleanup()
