<!--
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Security policy

## Supported versions

| Version | Security fixes |
|---|---|
| Latest `rpi4-v*` release | Supported |
| `raspi4/full-platform` | Supported until the next release |
| Older fork releases | Upgrade required |
| Unmodified upstream QEMU | Follow the upstream QEMU security process |

## Reporting a vulnerability

Report fork-specific vulnerabilities through GitHub private vulnerability
reporting for `perara/qemu`. Do not open a public issue for an uncoordinated
vulnerability and do not attach private keys, credentials, proprietary
firmware, or sensitive hardware captures.

Include:

- affected commit or release;
- host and guest configuration;
- minimal reproduction steps;
- security impact and trust boundary;
- whether physical hardware is required;
- logs scrubbed of credentials and unique device secrets.

Reports concerning unmodified upstream QEMU should follow the
[QEMU security process](https://www.qemu.org/security/).

## Security boundary

Behavioral boot validation does not establish electrical, analogue, RF,
cycle-accurate, physical-fuse, side-channel, or silicon root-of-trust
conformance. Those claims require the explicit Pass 2 HIL gates documented in
`RASPI4_IMPLEMENTATION_MATRIX.md`.
