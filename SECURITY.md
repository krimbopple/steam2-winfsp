# Security Policy

## Supported versions

Until the first stable release, security fixes target the latest commit on the default branch and the latest tagged release.

## Reporting a vulnerability

Use GitHub private vulnerability reporting from the repository's **Security** tab. Please do not open a public issue for vulnerabilities involving:

- malformed blob/manifest/checksum parsing;
- path traversal or projected-path collisions;
- decompression or allocation limits;
- arbitrary local file access through build definitions;
- WinFsp callback memory safety;
- privilege or process-launch behavior.

Include a minimal synthetic reproducer where possible. Do not upload copyrighted game payloads, private depot archives, credentials, or API keys.

If private reporting is unavailable, open a public issue requesting a private contact channel without disclosing exploit details.

## Scope

The project does not download or distribute game content and does not install WinFsp. Vulnerabilities in upstream WinFsp or Qt should also be reported to their respective projects when appropriate.
