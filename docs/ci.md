# CI/CD

The [macOS build workflow](../.github/workflows/macos.yml) runs on pushes to `main`, pull requests targeting `main`, version tags and manual dispatch. It uses an Apple Silicon `macos-15` runner to build and package with `BUILD_TESTING=OFF` and `OSCILLOSCOPE_DEV_PROBES=OFF`. It does not run tests, benchmarks, host checks or installation.

Each successful run uploads `macOS-arm64-development-<commit>` for 14 days. Download it from the run's **Artifacts** section. It contains a development `.pkg`, a `.zip` with installation instructions, and `SHA256SUMS`. These packages use ad-hoc plugin signing and unsigned installers; they are not notarized production releases.

## Draft development releases

When ready to prepare a draft, update `VERSION`, commit it and push a matching tag, for example `v0.2.0`. The workflow refuses tags that do not match `VERSION`. After packaging succeeds, a separate job attaches the development artifacts to a **draft prerelease**. Review it in GitHub Releases before deciding whether to publish it. Tag creation itself does not publish a release.

Only the tag-only draft job receives `contents: write`. Build and pull request jobs use read-only repository access, do not receive signing secrets and do not retain checkout credentials. Official GitHub Actions are pinned to full commit IDs. Branch and tag values enter shell commands through environment variables.

## Production releases

Production signing credentials are not provisioned in this workflow. Before automating production distribution, configure a protected GitHub environment with Developer ID Application/Installer certificates, their private keys and notary credentials. Import them into a temporary keychain, run `scripts/package-macos.sh --mode release`, then delete the keychain. Require explicit approval for public publication and follow [the distribution guide](distribution.md). Never store credentials, certificate exports or notarization responses in Git.
