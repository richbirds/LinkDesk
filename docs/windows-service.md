# Windows unattended host

The interactive client remains an ordinary user application. Its existing host
mode is retained unless an administrator installs the machine host from Software
Settings. Installation requires explicit unattended authorization and a machine
connection password. Windows account credentials are never stored by LinkDesk.

## Components and trust boundaries

- `LinkDeskService.exe`: SCM automatic-start LocalSystem service, machine relay
  registration and end-to-end TLS/password authentication. Identity, certificates
  and sealed policy live in ProgramData/LinkDesk/Host, accessible only to SYSTEM
  and Administrators. The machine password is distinct from Windows credentials.
- `LinkDeskWorker.exe --capture`: restricted SYSTEM token in the selected
  interactive session. A dedicated native thread opens and binds the current
  input desktop before creating capture or input resources. When the input
  desktop differs from the process startup desktop, the supervisor replaces the
  capture worker in the fixed Default or explicitly authorized Winlogon desktop;
  a thread rebind alone does not establish secure-desktop input access. The service does not
  capture Session 0. Worker loss invalidates cached images and restarts the worker. A bounded, image-coalescing mailbox serializes worker events on the supervisor thread; generation checks discard events from replaced workers.
- `LinkDeskWorker.exe --user-broker`: the logged-on user's limited token, with
  medium integrity. File access uses that user's download/settings directory;
  file access is unavailable before login. This process never receives machine
  credentials and cannot launch arbitrary programs.
- `LinkDeskServiceSetup.exe`: a separate elevated policy/installation utility.
  The full GUI is not elevated. Service binaries and dependencies are installed
  under Program Files with protected administrator-owned ACLs.

Named pipes reject remote clients, have explicit ACLs and validate service PID,
client PID/session/token or impersonated caller identity. The public local
control protocol permits status, pause, resume and disconnect only. It cannot
supply executable paths, shell commands, desktop names or machine policy.
Administrator configuration is an explicit elevated setup operation, not an IPC
command. Private worker pipes use unpredictable names and fixed bounded messages.

## Session and desktop transitions

The default policy selects the console. The optional active-session policy
prefers an active console and otherwise follows an active RDP session. It does
not attach to all users simultaneously. Session changes terminate the old job
before starting its replacement. No input is accepted until a new desktop frame
is available. Injected key/button state is released on transitions/disconnects. Release acknowledgments carry an epoch and serial; the replacement worker must acknowledge outstanding releases before new input or images are exposed.

A desktop boundary releases duplication/GDI and encoder references, invalidates
pending input and clears viewer/thumbnail image and decoder state. The next image
is independent of the preceding desktop. A service host requires a viewer that
supports this boundary protocol; older viewers must be upgraded.

Explicit unattended authorization covers login, lock and UAC capture/input.
Files, power operations and SAS each have separate policy grants. Clicking Quit
in the ordinary GUI does not stop the machine host. Stop/uninstall requires
administrator authorization and closes the worker jobs. Uninstall retains the
protected machine identity/policy so reinstall can preserve it.

Service pause/resume is stored in the protected machine directory and survives
restarts; resuming never grants unattended access if administrator policy denies
it. The GUI's login startup option is independent of SCM boot startup.

Updates stage and validate the fixed component set before stopping the service,
then replace components atomically per file with a rollback copy. An interrupted
installation leaves a protected marker that prevents the service from loading
credentials until an administrator repairs the interrupted transaction. Setup
refuses to discard an outstanding marker or rollback directory automatically. Existing machine data
is retained through update and uninstall.

## Identity migration

The setup wizard can retain the user's original device ID or create a separate
machine ID. Retaining an ID requires the relay's credential-migration protocol:
setup journals a fresh machine token in the protected directory, the relay
atomically rotates the registration credential, and the service finalizes its
machine identity. Retries are idempotent. The previous user token is revoked;
the ordinary viewing client uses a separate personal registration. A fresh
machine TLS private key is generated, rather than copying the user's key. User
DPAPI files are never copied to the service. Never delete a pending migration
journal merely to bypass an unavailable relay.

## Secure attention sequence

The viewer's SAS button and Ctrl+Alt+Delete shortcut request Windows `SendSAS`
through the authorized service. Ordinary SendInput is not a SAS implementation.
The service must have the explicit SAS capability, and Windows' “Disable or
enable software Secure Attention Sequence” policy must allow services
(`SoftwareSASGeneration` 1 or 3). LinkDesk does not weaken or change this policy.
If unavailable, use the login screen's supported alternate sign-in flow or have
an administrator configure the appropriate policy. The Microsoft service path
is separate from the `uiAccess` application path, whose signing and protected
installation requirements must not be bypassed. Production binaries should be
signed and their publisher verified during elevation.

References:
- https://learn.microsoft.com/en-us/windows/win32/api/sas/nf-sas-sendsas
- https://learn.microsoft.com/en-us/windows/win32/api/wtsapi32/nf-wtsapi32-wtsqueryusertoken
- https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setthreaddesktop

## Regression procedure

Use a disposable Windows VM first; keep all credentials, logs, screenshots,
dumps and test descriptors outside source control.

1. Install through administrator setup; check SCM automatic startup, Session 0
   service, selected-session worker and ordinary-user file broker tokens/ACLs.
2. Cold boot without automatic login; verify registration and login-screen image.
   Type Windows credentials manually through the remote viewer.
3. Lock/unlock, log off/on, fast user switching, console changes, UAC and
   monitor sleep: verify identity stability, clear-before-first-frame behavior,
   automatic recovery and no stuck keys or duplicate workers.
4. Wrong remote password, old viewer boundary capability and unauthorized local
   tokens must not activate capture/input. Unknown IPC commands must be rejected.
5. Terminate the capture worker and verify replacement and same authenticated
   connection recovery; test file transfer/resume under the ordinary user.
6. Stop/uninstall with a connected viewer; ensure no worker/broker processes
   remain and protected identity is retained. Reinstall and verify same ID.
7. RDP switching is a separate optional environment-specific test; it is not part of the current console-VM validation.
8. Re-run ordinary Mac/Windows host/viewer, thumbnail/full-window input, file
   transfer, settings, tray, startup and complete-exit tests.

BitLocker preboot, firmware/BIOS and other stages before Windows runs are outside
the scope of a Windows service. A virtual display driver is a separate solution
for display availability and does not replace session or desktop authorization.
