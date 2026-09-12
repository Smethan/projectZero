# Fork firmware releases

Smethan/projectZero **main** includes the former feature/all-wardrive history.
GitHub's default branch remains main. Old feature branches remain as checkpoints.

The only active workflow is `Fork firmware release`. Pushing a numeric version
tag (for example `v1.7.1`) runs the native transport tests, builds standard and
XIAO ESP32-C5 firmware, packages manifests/checksums and publishes a GitHub Release.
The tag must match `JANOS_VERSION` in `ESP32C5/main/main.c`; CMake reads the same
value into the binary metadata. Update `docs/RELEASE_NOTES.md` before tagging.

```sh
git switch main
git pull --ff-only
# Edit JANOS_VERSION and release notes; commit and push the source changes.
git tag v1.7.3
git push origin v1.7.3
```

A manual workflow dispatch on main builds and checks both boards without
publishing a release. To retry a failed tagged build, rerun that Actions run.
Publication stages all assets in a draft before marking the release latest;
it refuses to overwrite an already-published release.

Both repositories must stay public for this free-runner arrangement. The workflow
refuses private repositories, uses only standard `ubuntu-24.04` runners, has a
45-minute timeout, and uses no larger runners, Actions caches or retained
Actions artifacts. Build outputs go into GitHub Releases instead. No PAT,
external service, Discord webhook, Pages deployment or paid integration is needed;
the publish job uses its scoped `GITHUB_TOKEN` with contents:write.

The ESP-IDF container is pinned by digest. Each board ZIP contains its own
bootloader, partition table, initial OTA data, application and `manifest.json`.
The manifest records board, version, source commit, offsets, sizes and hashes.
`SHA256SUMS` covers all downloadable ZIPs, standalone applications and build logs.

WDG uses the board ZIP and validates it before touching serial. Onboard stable
or tagged OTA downloads the appropriate standalone app from this same fork.
The old development-branch binary channel is disabled. Existing devices need
one installation of this fork-update-capable build before onboard OTA follows
Smethan; updating WDG alone changes WDG's serial flasher source.

Free-tier basis checked September 2026:
- https://docs.github.com/en/billing/concepts/product-billing/github-actions
- https://docs.github.com/en/repositories/releasing-projects-on-github/about-releases
