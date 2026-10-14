# LinkDesk project instructions

## User preferences

- Maintain Mac and Windows clients with shared code.
- Save Mac connection passwords locally with encryption; never use Keychain password APIs or request Keychain access.
- App icon uses a light warm palette, sage/apricot accents, and no blue background.
- Keep full desktop bandwidth uncapped; low-bandwidth thumbnails are intentional.

## GitHub synchronization

The user explicitly authorized publishing reviewed project updates to https://github.com/richbirds/LinkDesk and syncing each future major feature version (for example 0.7.0 or 1.0.0). Patch-only releases do not require a sync unless requested.

After a major feature version: complete applicable validation, review the exact staged file list and diff, run `python3 scripts/check_publish.py --staged`, then commit and push to the existing main branch. Verify the remote commit. Use a GitHub noreply author identity for local commits. Preserve remote history; never force push. Use the authenticated GitHub connector if local Git credentials are unavailable. If the branch is protected, create a pull request and attach it to the task.

Do not create a scheduled automation for this workflow. It applies when doing an actual release in this project.

## Privacy gate

Never publish real server configs, enrollment keys, private keys/certificates, SSH identities, proxy credentials, saved passwords, device identity databases, connection logs, screenshots of real desktops, personal deployment/testing records, local SDK paths, or preconfigured client bundles. All generated build/dist/archive directories stay local. Public examples use placeholders.

Respect the allowlist in `.gitignore`. Do not bypass ignores with `git add -f`. Stage only the explicit reviewed public files; do not use `git add .` for releases. New directories need deliberate review before adding to the allowlist. Keep local build paths in ignored `.local-build.env`.
