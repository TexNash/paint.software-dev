# paint.software — TexNash Edition

This branch contains a personally maintained version of [paint.software](https://github.com/Univers4craft/paint.software), an open-source, Qt6-based alternative to Paint.NET for Linux.

It includes additional features, UI changes and improvements that may not be present in the original application.

This is an **unofficial downstream version**, maintained independently of the original project.

## Changes from upstream

- Additional UI refinements
- Removal of redundant dialog preview images
- Other personal enhancements as they are implemented

## Build from source

The following instructions build the TexNash version rather than the original upstream application.

### Debian, Ubuntu and Linux Mint

Install build dependencies:

```bash
sudo apt install build-essential cmake qt6-base-dev libgl1-mesa-dev
```

Clone the `myfeatures` branch:

```bash
git clone --branch myfeatures --single-branch https://github.com/TexNash/paint.software-dev.git
cd paint.software-dev
```

Configure, compile and run:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/paintdotnet
```

## Binary releases

Precompiled Debian packages, AppImages and Flatpak bundles will be documented here when available.

## Original project

The original paint.software project is developed and maintained by [Univers4craft](https://github.com/Univers4craft/paint.software).

The TexNash edition remains based on that project and incorporates upstream improvements where appropriate.

Distributed under the project's existing MIT licence.
