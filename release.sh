#!/usr/bin/env bash
#
# release.sh - build, package and (optionally) publish an iGame release
#
# Usage:
#   ./release.sh <version>             build and package only, into dist/
#   ./release.sh <version> --publish   also tag the commit, push the tag and
#                                      create a draft GitHub release
#
#   <version> looks like 2.7.0
#
# Requirements:
#   - Docker, running (builds use the walkero/docker4amigavbcc image)
#   - for --publish: the GitHub CLI, logged in (brew install gh; gh auth login)
#
# The release is created as a draft: it stays invisible until you press
# "Publish release" on GitHub, so you can check it first.
#
# Settings, through environment variables:
#   RELEASE_NAME   name in the release title (default: iGame)
#   CPUS           CPU variants to build (default: "000 030 040 060")
#
set -euo pipefail

IMAGE="walkero/docker4amigavbcc:latest-m68k"
RELEASE_NAME="${RELEASE_NAME:-iGame}"
CPUS="${CPUS:-000 030 040 060}"

cd "$(dirname "$0")"
ROOT="$(pwd)"

die()  { echo "error: $*" >&2; exit 1; }
info() { echo "==> $*"; }

usage()
{
	sed -n '3,22p' "$0" | sed 's/^# \{0,1\}//'
	exit 1
}

# ---------------------------------------------------------------------------
# Arguments
# ---------------------------------------------------------------------------

VERSION=""
PUBLISH=0
for arg in "$@"; do
	case "$arg" in
		--publish) PUBLISH=1 ;;
		-h|--help) usage ;;
		-*) die "unknown option $arg" ;;
		*) [ -z "$VERSION" ] || die "only one version, please"; VERSION="$arg" ;;
	esac
done

[ -n "$VERSION" ] || usage
echo "$VERSION" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' || die "the version must look like 2.7.0, not '$VERSION'"

TAG="v$VERSION"
PKG="iGame-$TAG"                       # the drawer inside the archive
DATESTR="$(date +%Y%m%d)"
ARCHIVE="$PKG-$DATESTR.lha"
DIST="$ROOT/dist"

# ---------------------------------------------------------------------------
# Checks
# ---------------------------------------------------------------------------

docker info >/dev/null 2>&1 || die "Docker is not running"

if [ "$PUBLISH" = 1 ]; then
	command -v gh >/dev/null 2>&1 || die "the GitHub CLI is missing: brew install gh, then gh auth login"
	gh auth status >/dev/null 2>&1 || die "the GitHub CLI is not logged in: gh auth login"

	# The tag must point at exactly the source the binaries are built from
	if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
		die "there are uncommitted changes. Commit them first, so the release matches its source"
	fi
	if git rev-parse -q --verify "refs/tags/$TAG" >/dev/null; then
		die "tag $TAG exists already"
	fi
fi

# Runs a command in the build image, in this folder
in_docker()
{
	docker run --rm --platform linux/amd64 -v "$ROOT":/work -w /work "$IMAGE" sh -c "$1"
}

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------

info "Building $RELEASE_NAME $VERSION"

for cpu in $CPUS; do
	info "  CPU $cpu"
	# Rebuild from scratch, so the version is compiled into everything
	rm -f src/*_"$cpu".o "iGame.$cpu"
	in_docker "make -f Makefile.docker CPU=$cpu DRONE_TAG=$TAG" > "build-$cpu.log" 2>&1 \
		|| { tail -20 "build-$cpu.log"; die "the $cpu build failed, see build-$cpu.log"; }
	[ -f "iGame.$cpu" ] || die "iGame.$cpu was not created, see build-$cpu.log"
	rm -f "build-$cpu.log"
done

info "  Catalogs"
in_docker "make -f Makefile.docker catalogs" > build-catalogs.log 2>&1 \
	|| { tail -20 build-catalogs.log; die "building the catalogs failed, see build-catalogs.log"; }
rm -f build-catalogs.log

# ---------------------------------------------------------------------------
# Package. Everything is copied into dist/, the sources stay untouched
# ---------------------------------------------------------------------------

info "Packaging $ARCHIVE"

rm -rf "$DIST/$PKG" "$DIST/$PKG.info" "$DIST/$ARCHIVE"
mkdir -p "$DIST/$PKG"
STAGE="$DIST/$PKG"

cp -R required_files/. "$STAGE/"

# Drawer icons: the 3.0 style one becomes the drawer's icon, both
# styles are kept in extras/icons for those who prefer the other
mkdir -p "$STAGE/extras/icons"
cp "$STAGE/igame_drawer_3.0.info" "$DIST/$PKG.info"
mv "$STAGE/igame_drawer_3.0.info" "$STAGE/igame_drawer.info" "$STAGE/extras/icons/"

# The genres list, wherever it lives at the moment
if [ ! -f "$STAGE/genres" ]; then
	[ -f genres ] && cp genres "$STAGE/genres" || echo "warning: no genres file found" >&2
fi

# Version and date in the guide and the changelog
RELEASE_DATE_GUIDE="$(date +%d.%m.%Y)"
RELEASE_DATE_LOG="$(date +%Y-%m-%d)"
sed -e "s/VERSION_TAG/$TAG/g" -e "s/RELEASE_DATE/$RELEASE_DATE_GUIDE/g" \
	required_files/iGame.guide > "$STAGE/iGame.guide"
sed -e "s/VERSION_TAG/$TAG/g" -e "s/RELEASE_DATE/$RELEASE_DATE_LOG/g" \
	CHANGELOG.md > "$STAGE/CHANGELOG.md"

# Licence and readme
cp COPYING "$STAGE/COPYING"
cp README.md "$STAGE/README.md"

# Catalogs: the source description and every compiled language
mkdir -p "$STAGE/catalogs"
cp catalogs/iGame.cd "$STAGE/catalogs/"
for catalog in catalogs/*/iGame.catalog; do
	[ -f "$catalog" ] || continue
	lang="$(basename "$(dirname "$catalog")")"
	mkdir -p "$STAGE/catalogs/$lang"
	cp "$catalog" "$STAGE/catalogs/$lang/"
done

# Binaries: the 68000 build is the plain "iGame" that the icon starts
cp iGame.000 "$STAGE/iGame"
for cpu in $CPUS; do
	[ "$cpu" = 000 ] || cp "iGame.$cpu" "$STAGE/"
done

# Leave out host and emulator leftovers
find "$DIST" \( -name '.DS_Store' -o -name '*.uaem' -o -name '._*' \) -exec rm -f {} +

# The archive, with LhA from the build image
docker run --rm --platform linux/amd64 -v "$DIST":/dist -w /dist "$IMAGE" \
	sh -c "lha -aq2o6 '$ARCHIVE' '$PKG/' '$PKG.info'" \
	|| die "creating the archive failed"

info "Created dist/$ARCHIVE ($(du -h "$DIST/$ARCHIVE" | cut -f1 | tr -d ' '))"

# ---------------------------------------------------------------------------
# Publish
# ---------------------------------------------------------------------------

if [ "$PUBLISH" = 0 ]; then
	echo
	echo "Nothing was published. To tag and upload this release, run:"
	echo "  ./release.sh $VERSION --publish"
	exit 0
fi

# Release notes: the first section of the changelog, without its heading
NOTES="$DIST/$PKG-notes.md"
awk 'NR > 1 && /^## / { exit } NR > 1 { print }' "$STAGE/CHANGELOG.md" > "$NOTES"
[ -s "$NOTES" ] || echo "$RELEASE_NAME $VERSION" > "$NOTES"

REPO="$(gh repo view --json nameWithOwner -q .nameWithOwner)"

echo
echo "About to publish:"
echo "  repository : $REPO"
echo "  tag        : $TAG on $(git rev-parse --short HEAD) ($(git log -1 --format=%s))"
echo "  release    : \"$RELEASE_NAME $VERSION\" (draft)"
echo "  file       : $ARCHIVE"
echo
printf "Push the tag and create the draft release? [y/N] "
read -r answer
case "$answer" in
	y|Y|yes|YES) ;;
	*) echo "Stopped. Nothing was published."; exit 0 ;;
esac

info "Tagging $TAG"
git tag -a "$TAG" -m "$RELEASE_NAME $VERSION"
git push origin "$TAG"

info "Creating the draft release"
gh release create "$TAG" "$DIST/$ARCHIVE" \
	--repo "$REPO" \
	--title "$RELEASE_NAME $VERSION" \
	--notes-file "$NOTES" \
	--draft

echo
echo "Done. The release is a draft: check it and press \"Publish release\" on"
echo "  https://github.com/$REPO/releases"
