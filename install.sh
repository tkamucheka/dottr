#!/bin/sh
# Installs dottr: the programs dottr, dottr-format, and dottr-lsp, into
# ~/.local/opt/dottr/bin, with symbolic links in ~/.local/bin.
#
#   curl -fsSL https://raw.githubusercontent.com/tkamucheka/dottr/main/install.sh | sh
#
# Options follow `sh -s --`; run with --help for the list. By default the
# script installs the release archive for the platform, verified against the
# SHA256SUMS of the release. It builds from source instead when there is no
# archive for the platform, or when --ref, --repo, or --source asks for a
# source. The source is downloaded with git if it is installed, and otherwise
# as a GitHub archive with curl or wget and tar. The compiler is $CC
# (default: cc) with $CFLAGS (default: -O2).
#
# The script is a single function that is called on its last line, so that
# a partially downloaded script does nothing.

set -eu

DEFAULT_REPO=https://github.com/tkamucheka/dottr.git
RELEASES=${DOTTR_RELEASES:-https://github.com/tkamucheka/dottr/releases}
PROGRAMS="dottr dottr-format dottr-lsp"

say() {
    printf 'dottr-install: %s\n' "$*"
}

die() {
    printf 'dottr-install: error: %s\n' "$*" >&2
    exit 1
}

usage() {
    cat <<'USAGE'
usage: install.sh [OPTION...]
       curl -fsSL URL | sh -s -- [OPTION...]

Installs dottr, dottr-format, and dottr-lsp from a release archive, or
builds them from source.

options (environment variable in parentheses):
  --version TAG  release to install (DOTTR_VERSION; default: the latest;
                 DOTTR_RELEASES sets the base URL of the releases)
  --archive FILE install a downloaded release archive
  --ref REF      build the branch, tag, or commit REF from source (DOTTR_REF)
  --repo URL     build from the git repository URL (DOTTR_REPO;
                 default: https://github.com/tkamucheka/dottr.git)
  --source DIR   build from a local checkout
  --prefix DIR   installation directory (DOTTR_PREFIX;
                 default: ~/.local/opt/dottr)
  --bin-dir DIR  directory of the links to the programs (DOTTR_BIN_DIR;
                 default: ~/.local/bin)
  --no-link      do not create the links
  -h, --help     show this help
USAGE
}

have() {
    command -v "$1" >/dev/null 2>&1
}

# download URL FILE: downloads URL into FILE. Fails quietly, so that a
# missing release can fall back to a source build.
download() {
    if have curl; then
        curl -fsSL -o "$2" "$1" 2>/dev/null
    elif have wget; then
        wget -q -O "$2" "$1" 2>/dev/null
    else
        die "curl or wget is required to download dottr"
    fi
}

# platform: prints the name of the release archive for this system, or
# nothing if there is none.
platform() {
    case $(uname -s) in
    Linux)
        case $(uname -m) in
        x86_64 | amd64) echo linux-x86_64 ;;
        aarch64 | arm64) echo linux-aarch64 ;;
        esac
        ;;
    Darwin) echo macos-universal ;;
    esac
}

# sha256 FILE: prints the SHA-256 digest of FILE.
sha256() {
    if have sha256sum; then
        sha256sum "$1" | cut -d' ' -f1
    elif have shasum; then
        shasum -a 256 "$1" | cut -d' ' -f1
    else
        die "sha256sum or shasum is required to verify the download"
    fi
}

# fetch_release VERSION PLATFORM DIR: downloads and verifies the release
# archive into DIR/archive.tar.gz. Returns 1 if the release has no archive
# for the platform.
fetch_release() {
    fr_version=$1 fr_platform=$2 fr_dir=$3
    fr_asset=dottr-$fr_platform.tar.gz
    if [ "$fr_version" = latest ]; then
        fr_base=$RELEASES/latest/download
    else
        fr_base=$RELEASES/download/$fr_version
    fi
    say "downloading $fr_base/$fr_asset"
    download "$fr_base/$fr_asset" "$fr_dir/archive.tar.gz" || return 1
    download "$fr_base/SHA256SUMS" "$fr_dir/SHA256SUMS" ||
        die "the release has no SHA256SUMS"
    fr_expected=$(awk -v name="$fr_asset" '$2 == name || $2 == "*" name { print $1 }' \
        "$fr_dir/SHA256SUMS")
    [ -n "$fr_expected" ] || die "SHA256SUMS does not list $fr_asset"
    [ "$(sha256 "$fr_dir/archive.tar.gz")" = "$fr_expected" ] ||
        die "the checksum of $fr_asset does not match SHA256SUMS"
}

# fetch_git REPO REF DIR: downloads the repository at a ref into DIR, which
# must not exist. (POSIX sh has no local variables, hence the prefixes.)
fetch_git() {
    fg_repo=$1 fg_ref=$2 fg_dir=$3
    if git clone --quiet --depth 1 --branch "$fg_ref" "$fg_repo" "$fg_dir" \
        2>/dev/null; then
        return 0
    fi
    # A commit cannot be cloned with --branch; clone everything instead.
    rm -rf "$fg_dir"
    git clone --quiet "$fg_repo" "$fg_dir" ||
        die "cannot clone $fg_repo (for a private repository, pass an SSH URL with --repo)"
    git -C "$fg_dir" -c advice.detachedHead=false checkout --quiet "$fg_ref" \
        2>/dev/null ||
        die "$fg_repo has no branch, tag, or commit named '$fg_ref'"
}

# fetch_archive REPO REF DIR: downloads a GitHub archive of the ref into DIR,
# which must exist and be empty.
fetch_archive() {
    fa_repo=$1 fa_ref=$2 fa_dir=$3
    case $fa_repo in
    https://github.com/*) ;;
    *) die "git is not installed, and $fa_repo is not a GitHub https URL" ;;
    esac
    fa_url="${fa_repo%.git}/archive/$fa_ref.tar.gz"
    say "downloading $fa_url"
    if have curl; then
        curl -fsSL "$fa_url"
    elif have wget; then
        wget -qO- "$fa_url"
    else
        die "git, curl, or wget is required to download dottr"
    fi | tar -xzf - -C "$fa_dir" --strip-components=1 ||
        die "cannot download $fa_url"
}

# unpack ARCHIVE DIR: extracts the programs of a release archive into
# DIR/bin. The archive holds one directory with bin/ inside.
unpack() {
    mkdir "$2/archive"
    tar -xzf "$1" -C "$2/archive" || die "cannot extract $1"
    for program in $PROGRAMS; do
        found=$(find "$2/archive" -path "*/bin/$program" -type f | head -n 1)
        [ -n "$found" ] || die "$1 does not contain bin/$program"
        cp "$found" "$2/bin/$program"
    done
}

# build REPO REF SOURCE DIR: downloads REPO at REF, or uses the checkout
# SOURCE, and builds the programs into DIR/bin. Every program is built
# before any is installed.
build() {
    b_repo=$1 b_ref=$2 b_source=$3 b_dir=$4
    cc=${CC:-cc}
    cflags=${CFLAGS:--O2}
    have "$cc" || die "C compiler '$cc' not found; install one or set CC"

    if [ -n "$b_source" ]; then
        [ -f "$b_source/tools/dottr/dottr.c" ] ||
            die "$b_source is not a dottr checkout"
        src=$b_source
        say "building from $src"
    else
        src=$b_dir/src
        if have git; then
            say "downloading $b_repo ($b_ref)"
            fetch_git "$b_repo" "$b_ref" "$src"
        else
            mkdir "$src"
            fetch_archive "$b_repo" "$b_ref" "$src"
        fi
        if [ -d "$src/.git" ]; then
            say "revision $(git -C "$src" rev-parse --short HEAD)"
        fi
    fi

    # CFLAGS are split into words on purpose.
    say "building with $cc $cflags"
    for program in $PROGRAMS; do
        (
            cd "$src"
            # shellcheck disable=SC2086
            "$cc" -std=c11 $cflags -Iinclude lib/*.c \
                "tools/$program/$program.c" -o "$b_dir/bin/$program"
        ) || die "build of $program failed"
    done
}

main() {
    repo=${DOTTR_REPO:-}
    ref=${DOTTR_REF:-}
    version=${DOTTR_VERSION:-latest}
    prefix=${DOTTR_PREFIX:-}
    bin_dir=${DOTTR_BIN_DIR:-}
    link=yes
    source_dir=
    archive=

    while [ $# -gt 0 ]; do
        case $1 in
        --repo | --ref | --version | --archive | --prefix | --bin-dir | --source)
            [ $# -ge 2 ] || die "option '$1' requires an argument"
            case $1 in
            --repo) repo=$2 ;;
            --ref) ref=$2 ;;
            --version) version=$2 ;;
            --archive) archive=$2 ;;
            --prefix) prefix=$2 ;;
            --bin-dir) bin_dir=$2 ;;
            --source) source_dir=$2 ;;
            esac
            shift 2
            ;;
        --no-link)
            link=no
            shift
            ;;
        -h | --help)
            usage
            exit 0
            ;;
        *)
            die "unknown option '$1' (see --help)"
            ;;
        esac
    done

    [ -n "${HOME:-}" ] || die "HOME is not set"
    prefix=${prefix:-$HOME/.local/opt/dottr}
    bin_dir=${bin_dir:-$HOME/.local/bin}
    # A repository, a ref, or a checkout asks for a build from source.
    from_source=no
    if [ -n "$repo$ref$source_dir" ]; then
        from_source=yes
    fi
    repo=${repo:-$DEFAULT_REPO}
    ref=${ref:-main}

    if [ "$link" = yes ]; then
        for program in $PROGRAMS; do
            target=$bin_dir/$program
            if [ -e "$target" ] && [ ! -L "$target" ]; then
                die "$target exists and is not a symbolic link; remove it or use --no-link"
            fi
        done
    fi

    tmp=$(mktemp -d "${TMPDIR:-/tmp}/dottr-install.XXXXXX")
    trap 'rm -rf "$tmp"' EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM
    mkdir "$tmp/bin"

    # 1. Obtain the programs in $tmp/bin: from a release archive if possible.
    if [ -n "$archive" ]; then
        [ -f "$archive" ] || die "$archive does not exist"
        say "installing $archive"
        unpack "$archive" "$tmp"
    elif [ "$from_source" = no ]; then
        target_platform=$(platform)
        if [ -z "$target_platform" ]; then
            say "no release archive for $(uname -s) $(uname -m); building from source"
            from_source=yes
        elif fetch_release "$version" "$target_platform" "$tmp"; then
            unpack "$tmp/archive.tar.gz" "$tmp"
        elif [ "$version" = latest ]; then
            say "no release archive was found; building from source"
            from_source=yes
        else
            die "release $version has no archive for $target_platform"
        fi
    fi
    if [ "$from_source" = yes ] && [ -z "$archive" ]; then
        build "$repo" "$ref" "$source_dir" "$tmp"
    fi
    for program in $PROGRAMS; do
        "$tmp/bin/$program" --help >/dev/null ||
            die "$program does not run on this system"
    done

    # 2. Install. Each binary is replaced by a rename, so a running program is
    # not affected and an interrupted install leaves the old binary in place.
    mkdir -p "$prefix/bin"
    for program in $PROGRAMS; do
        cp "$tmp/bin/$program" "$prefix/bin/.$program.new"
        chmod 755 "$prefix/bin/.$program.new"
        mv -f "$prefix/bin/.$program.new" "$prefix/bin/$program"
        say "installed $prefix/bin/$program"
    done

    # 3. Link.
    if [ "$link" = yes ]; then
        mkdir -p "$bin_dir"
        for program in $PROGRAMS; do
            ln -sf "$prefix/bin/$program" "$bin_dir/$program"
            say "linked $bin_dir/$program -> $prefix/bin/$program"
        done
        case ":${PATH:-}:" in
        *":$bin_dir:"*) ;;
        *) say "note: $bin_dir is not in PATH; add it to run the programs" ;;
        esac
    fi
}

main "$@"
