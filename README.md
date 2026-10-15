<!-- SPDX-FileCopyrightText: 2026 maninblack -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Brake Health Service

Independent in-vehicle Brake Health QM service. It consumes authorized telemetry, produces analytics/advisory, and does not control braking.

<a id="sdv-lab-entry"></a>

For the **whole demo**, start at the [SDV Lab README](https://github.com/alexmaninblack/aosedge-sdv-demo).
Only the product repository is manually cloned for its pinned multi-component
build. The instructions below are for working on **this component alone**;
a host check does not publish, install or qualify a vehicle package.

## 1 Prepare a macOS component workspace

Use native Apple Silicon Terminal. These component commands are for development,
not a qualified full-demo installation. Run blocks in order and stop on error.

Choose an existing writable local APFS parent folder, on the internal disk or
an external SSD. Use the product launcher instead for automatic full-demo
preparation; this section is only the component workflow:

```sh
uname -m
printf 'Existing local APFS parent folder (absolute path): '
read -r SDV_PARENT
test -d "$SDV_PARENT" && test -w "$SDV_PARENT" && df -h "$SDV_PARENT"
```

Expect `arm64` and enough free space on the selected volume. Stop if the
folder/access check fails; do not create a replacement for a disconnected
volume. An external SSD is optional. After confirming storage:

```sh
SDV_WORK="$SDV_PARENT/sdv-components"
mkdir -p "$SDV_WORK"
export HOMEBREW_CACHE="$SDV_WORK/cache/homebrew"
```

Install Apple's Command Line Tools with `xcode-select --install` if missing,
and finish the system dialog. Install [Homebrew](https://docs.brew.sh/Installation)
if absent. Then:

```sh
eval "$(/opt/homebrew/bin/brew shellenv)"
brew install cmake python@3.12
export PATH="$(brew --prefix python@3.12)/libexec/bin:$PATH"
git --version
cmake --version
python3 --version
xcrun clang++ --version
```

Do not use the installed demo's private interpreter or a Rosetta toolchain.

## 2 Clone this component

```sh
git clone --branch main https://github.com/alexmaninblack/brake-health-service.git "$SDV_WORK/brake-health-service"
cd "$SDV_WORK/brake-health-service"
git rev-parse HEAD
```

Record the printed revision with your results. `main` is current development,
not a release pin. To reproduce the complete candidate, use the product
repository's manifest-driven route instead of independently choosing branches.

## 3 Build and check host targets

```sh
SDV_BUILD="$SDV_WORK/build/brake-health-service"
cmake -S . -B "$SDV_BUILD" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DBHS_BUILD_KUKSA_RUNTIME=OFF
cmake --build "$SDV_BUILD" --parallel 2
ctest --test-dir "$SDV_BUILD" --output-on-failure
```

Expect a successful build and no failed CTest cases. Keep the first failure;
do not replace expected results or lower resource/security requirements.

This C++17 build creates host/domain libraries, bootstrap and test programs.
It deliberately **does not build the real KUKSA service executable**. The
configuration reports that distinction; a bootstrap is not a deployable product.

For the repository's packaging/policy and additional host checks:

```sh
python3 -B -m unittest discover -s tests -p 'test_*.py'
python3 -B tools/quality_gate.py
```

The Python suite can compile additional isolated C++ fixtures. It is not a
no-build test command. Preserve its private temporary-file boundary rather
than globally changing `TMPDIR` for unrelated tools.

## 4 Produce the vehicle package

Use the product repository's pinned build chain or its Demo Control product
builder, following [Linux ARM64 product build](docs/product-build.md).
That route supplies the pinned gRPC/Protobuf/KUKSA dependencies and exports the
actual executable. Do not sign the historical diagnostic scaffold or run the
bootstrap on macOS with invented vehicle identity.

Brake profiles V1, V2 and V3 are functional content, distinct from allocated
release numbers. The integration owner signs, publishes and assigns them
serially; this component quickstart deploys nothing.

## 5 Finish

Build and test commands exit on completion; no long-running vehicle process
was started. Preserve results/source revision. Do not delete the installed
service's storage, keys or Test state to clean a host build.

## Component documentation

- [Architecture](docs/architecture.md)
- [Runtime profiles](docs/runtime-profiles.md)
- [Product build](docs/product-build.md)
- [Deployment gates](docs/runtime-executable.md)
- [Security](SECURITY.md), [license](LICENSE), [notices](NOTICE)

## Runtime reference

V1 captures brake events, V2 adds condition analysis, and V3 adds driver
advisory. These are functional content profiles, not historical release
numbers. All use the real packaged runtime, authorized KUKSA inputs and
independent durable backend delivery.

The [runtime profiles](docs/runtime-profiles.md) define composition and
the [product build](docs/product-build.md) defines the Linux ARM64 export.
The export requires the real executable, successful tests and checked native
dependencies; a diagnostic scaffold is not a deployable product.

These illustrative QM analytics do not control vehicle braking or steering.
Use the integration owner for signing, sequential publication and assignment.

## Security and license

Never commit KUKSA tokens, certificates, private keys or operational state.
See [SECURITY.md](SECURITY.md). Original work is Apache-2.0 under
`maninblack`; see [LICENSE](LICENSE), [NOTICE](NOTICE) and
[third-party notices](THIRD_PARTY_NOTICES.md).
