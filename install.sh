#!/bin/sh
# Installs dottr: downloads the source, builds it, and installs the binary
# into ~/.local/opt/dottr/bin, with a symbolic link in ~/.local/bin.
#
#   curl -fsSL https://raw.githubusercontent.com/tkamucheka/dottr/main/install.sh | sh
#
# Options follow `sh -s --`; run with --help for the list. The source is
# downloaded with git if it is installed, and otherwise as a GitHub archive
# with curl or wget and tar. The compiler is $CC (default: cc) with $CFLAGS
# (default: -O2).
#
# The script is a single function that is called on its last line, so that
# a partially downloaded script does nothing.

set -eu

DEFAULT_REPO=https://github.com/tkamucheka/dottr.git

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

Downloads, builds, and installs dottr.

options (environment variable in parentheses):
  --repo URL     git repository to download (DOTTR_REPO)
  --ref REF      branch, tag, or commit to build (DOTTR_REF; default: main)
  --prefix DIR   installation directory (DOTTR_PREFIX;
                 default: ~/.local/opt/dottr)
  --bin-dir DIR  directory of the dottr link (DOTTR_BIN_DIR;
                 default: ~/.local/bin)
  --no-link      do not create the link
  --source DIR   build from a local checkout instead of downloading
  -h, --help     show this help
USAGE
}

have() {
    command -v "$1" >/dev/null 2>&1
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

main() {
    repo=${DOTTR_REPO:-$DEFAULT_REPO}
    ref=${DOTTR_REF:-main}
    prefix=${DOTTR_PREFIX:-}
    bin_dir=${DOTTR_BIN_DIR:-}
    link=yes
    source_dir=

    while [ $# -gt 0 ]; do
        case $1 in
        --repo | --ref | --prefix | --bin-dir | --source)
            [ $# -ge 2 ] || die "option '$1' requires an argument"
            case $1 in
            --repo) repo=$2 ;;
            --ref) ref=$2 ;;
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
    cc=${CC:-cc}
    cflags=${CFLAGS:--O2}

    have "$cc" || die "C compiler '$cc' not found; install one or set CC"
    link_target=$bin_dir/dottr
    if [ "$link" = yes ] && [ -e "$link_target" ] && [ ! -L "$link_target" ]
    then
        die "$link_target exists and is not a symbolic link; remove it or use --no-link"
    fi

    tmp=$(mktemp -d "${TMPDIR:-/tmp}/dottr-install.XXXXXX")
    trap 'rm -rf "$tmp"' EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM

    # 1. Download.
    if [ -n "$source_dir" ]; then
        [ -f "$source_dir/tools/dottr/dottr.c" ] ||
            die "$source_dir is not a dottr checkout"
        src=$source_dir
        say "building from $src"
    else
        src=$tmp/src
        if have git; then
            say "downloading $repo ($ref)"
            fetch_git "$repo" "$ref" "$src"
        else
            mkdir "$src"
            fetch_archive "$repo" "$ref" "$src"
        fi
        if [ -d "$src/.git" ]; then
            say "revision $(git -C "$src" rev-parse --short HEAD)"
        fi
    fi

    # 2. Build. CFLAGS are split into words on purpose.
    say "building with $cc $cflags"
    (
        cd "$src"
        # shellcheck disable=SC2086
        "$cc" -std=c11 $cflags -Iinclude lib/*.c tools/dottr/dottr.c \
            -o "$tmp/dottr"
    ) || die "build failed"
    "$tmp/dottr" --help >/dev/null || die "the built binary does not run"

    # 3. Install. The binary is replaced by a rename, so a running dottr is
    # not affected and an interrupted install leaves the old binary in place.
    mkdir -p "$prefix/bin"
    cp "$tmp/dottr" "$prefix/bin/.dottr.new"
    chmod 755 "$prefix/bin/.dottr.new"
    mv -f "$prefix/bin/.dottr.new" "$prefix/bin/dottr"
    say "installed $prefix/bin/dottr"

    # 4. Link.
    if [ "$link" = yes ]; then
        mkdir -p "$bin_dir"
        ln -sf "$prefix/bin/dottr" "$link_target"
        say "linked $link_target -> $prefix/bin/dottr"
        case ":${PATH:-}:" in
        *":$bin_dir:"*) ;;
        *) say "note: $bin_dir is not in PATH; add it to run dottr" ;;
        esac
    fi
}

main "$@"
