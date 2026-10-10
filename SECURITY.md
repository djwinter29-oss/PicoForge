# Security Policy

## Supported Versions

Security fixes are developed against the latest code on the repository's default branch. Older snapshots and release artifacts are not currently maintained as separate supported versions.

## Reporting a Vulnerability

Please report suspected vulnerabilities privately through GitHub's **Report a vulnerability** feature on this repository's Security page. Do not open a public issue or pull request containing exploit details, credentials, private keys, device identifiers, or other sensitive data.

Include the affected revision or version, target board/MCU, prerequisites, impact, and a minimal reproduction where safe. Redact secrets and avoid testing against devices or systems you do not own. Maintainers will acknowledge reports and coordinate remediation and disclosure through the private report.

If private vulnerability reporting is unavailable for this repository, contact the maintainers privately through the GitHub account or organization that hosts it and request a secure reporting channel.

## Embedded-Specific Guidance

Do not commit production credentials, signing keys, customer data, or private device identifiers. The development USB VID/PID in this project is not a production identity. Hardware validation claims must state the board revision and test setup; host-only tests and cross-builds do not establish hardware security or electrical safety.
