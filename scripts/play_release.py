#!/usr/bin/env python3
"""Push the store listing, and promote a build, on Google Play.

Everything between "the AAB is built" and "the build is in front of users" is
API-writable, so none of it has to be done by hand in the Play Console:

    listing   title / short / full description + every screenshot
    release   put a versionCode on one or more tracks, at a rollout %, optionally
              uploading the AAB and pushing the listing in the same edit
    status    what is currently on each track

What this cannot do is the paperwork: the IARC content rating, the target-
audience and other App content declarations, category and pricing have no API
and are one-time console work. (Data safety does have an endpoint, but it takes
the console's own CSV export, so it is filled in there first.) See
docs/STORE-SETUP.md.

Play's edits are transactional: everything below opens an edit, changes it, and
commits once. Committing IS the submission -- there is no separate submit call,
which is why --dry-run stops before the commit rather than before the changes.

Play holds one set of changes in review at a time, and by default a commit
CANCELS whatever is in review and resubmits everything. That is fine by hand and
ruinous unattended: a merge every hour would restart review every hour and
nothing would ever clear it. --skip-if-busy commits with
changesInReviewBehavior=ERROR_IF_IN_REVIEW instead and exits 0 when Play
refuses -- the same contract as asc_release.py: the next release carries the
change. The one-slot rule is also why an unattended release does everything in
ONE edit: a listing commit followed by a release commit would find the listing
in review and skip the release. That is what --upload and --with-listing are for.

Credentials, first one set wins:
  $PLAY_SERVICE_ACCOUNT_JSON   the service-account JSON *content* (CI)
  ~/.config/openrackem/play-publisher.json

Usage:
  play_release.py status
  play_release.py listing [--dry-run]
  play_release.py release --build N --track TRACK [--track TRACK ...] \\
                          [--upload AAB] [--with-listing] [--rollout 0.2] \\
                          [--notes TEXT] [--skip-if-busy] [--dry-run]

TRACK is internal, alpha (closed testing), beta (open testing) or production.
`--build N` is the release number: the Makefile sets versionCode from it
(ANDROID_VERSION_CODE), so release-N is versionCode N on Play.
"""
import argparse
import base64
import json
import mimetypes
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding

sys.path.insert(0, str(Path(__file__).resolve().parent))
from store_listing import ListingError, play_listing  # noqa: E402

PACKAGE = "com.danheskett.openrackem"
SCOPE = "https://www.googleapis.com/auth/androidpublisher"
API = "https://androidpublisher.googleapis.com/androidpublisher/v3/applications"
UPLOAD = "https://androidpublisher.googleapis.com/upload/androidpublisher/v3/applications"
LANG = "en-US"
TRACKS = ["internal", "alpha", "beta", "production"]

# Release notes are the "What's new" text users see on a public track. A commit
# subject is written for developers, so releases carry the same deliberately
# boring standard line as asc_release.py. Pass --notes when a release warrants
# real copy.
DEFAULT_NOTES = "Various minor bug fixes & performance enhancements"

REPO = Path(__file__).resolve().parent.parent
ASSETS = REPO / "android/play-assets"

# Play image slots -> what fills them. The tablet screenshots are the same 9:16
# frames as the phone set at 2x, which both tablet slots accept.
IMAGES = {
    "icon":                 [ASSETS / "icon-512.png"],
    "featureGraphic":       [ASSETS / "feature-graphic-1024x500.png"],
    "phoneScreenshots":     sorted((ASSETS / "screenshots/phone").glob("*.png")),
    "sevenInchScreenshots": sorted((ASSETS / "screenshots/tablet").glob("*.png")),
    "tenInchScreenshots":   sorted((ASSETS / "screenshots/tablet").glob("*.png")),
}


def load_credential():
    """The service-account JSON itself, from $PLAY_SERVICE_ACCOUNT_JSON.

    One source, no fallback. A path on the developer's disk used to be tried
    next, so a run with the secret missing could still find some other key
    lying around and publish with it. Publishing is not a place to guess at
    credentials: either the variable is set, or this aborts.
    """
    raw = os.environ.get("PLAY_SERVICE_ACCOUNT_JSON")
    if not raw:
        sys.exit("PLAY_SERVICE_ACCOUNT_JSON is not set")
    return json.loads(raw)


def access_token(sa):
    b64 = lambda b: base64.urlsafe_b64encode(b).rstrip(b"=")
    now = int(time.time())
    signing_input = b64(json.dumps({"alg": "RS256", "typ": "JWT"}).encode()) + b"." + b64(
        json.dumps({"iss": sa["client_email"], "scope": SCOPE, "aud": sa["token_uri"],
                    "iat": now, "exp": now + 3600}).encode())
    key = serialization.load_pem_private_key(sa["private_key"].encode(), password=None)
    assertion = (signing_input + b"." + b64(
        key.sign(signing_input, padding.PKCS1v15(), hashes.SHA256()))).decode()
    body = urllib.parse.urlencode({
        "grant_type": "urn:ietf:params:oauth:grant-type:jwt-bearer",
        "assertion": assertion}).encode()
    with urllib.request.urlopen(urllib.request.Request(sa["token_uri"], data=body)) as r:
        return json.load(r)["access_token"]


class Play:
    def __init__(self, dry_run=False):
        self.token = access_token(load_credential())
        self.dry_run = dry_run
        self.edit = None

    def call(self, method, url, data=None, content_type="application/json", mutating=True):
        if self.dry_run and mutating:
            shown = ""
            if isinstance(data, (bytes, bytearray)) and content_type == "application/json":
                shown = " " + data.decode()[:400]
            elif isinstance(data, (bytes, bytearray)):
                shown = f" <{len(data)} bytes>"
            print(f"  [dry-run] {method} {url.replace(API, '')}{shown}")
            return {}
        headers = {"Authorization": "Bearer " + self.token}
        if data is not None:
            headers["Content-Type"] = content_type
        req = urllib.request.Request(url, data=data, method=method, headers=headers)
        try:
            with urllib.request.urlopen(req) as r:
                body = r.read()
            return json.loads(body) if body else {}
        except urllib.error.HTTPError as e:
            sys.exit(f"HTTP {e.code} on {method} {url}\n{e.read().decode()[:1500]}")

    # -- edit lifecycle -----------------------------------------------------
    def open_edit(self):
        # A dry run still opens a real edit: it is the only way to read current
        # state, and an edit that is never committed changes nothing.
        r = self.call("POST", f"{API}/{PACKAGE}/edits", data=b"", mutating=False)
        self.edit = r["id"]
        return self.edit

    def commit(self, skip_if_busy=False):
        """Commit the edit. False means Play was busy and skip_if_busy let it go."""
        if self.dry_run:
            print("  [dry-run] edit NOT committed; nothing changed on Play")
            return True
        url = f"{API}/{PACKAGE}/edits/{self.edit}:commit"
        if skip_if_busy:
            url += "?changesInReviewBehavior=ERROR_IF_IN_REVIEW"
        req = urllib.request.Request(url, data=b"", method="POST",
                                     headers={"Authorization": "Bearer " + self.token})
        try:
            urllib.request.urlopen(req).read()
        except urllib.error.HTTPError as e:
            body = e.read().decode()[:1500]
            # Only Play's refusal over a pending review is let go. Anything else
            # -- including "changes cannot be sent for review automatically",
            # which needs a human in the console -- must stay a failure, or it
            # would be skipped silently on every release forever. The edit is
            # left to expire on its own rather than deleted: a DELETE that fails
            # here would mask the message that matters.
            if skip_if_busy and "in review" in body.lower():
                print("  Play already has changes in review; leaving them alone and "
                      "skipping this edit")
                print("  " + body.strip().replace("\n", "\n  ")[:600])
                return False
            sys.exit(f"HTTP {e.code} on POST {url}\n{body}")
        print(f"  edit {self.edit} committed")
        return True

    def abandon(self):
        if self.edit and not self.dry_run:
            self.call("DELETE", f"{API}/{PACKAGE}/edits/{self.edit}", mutating=False)

    def base(self):
        return f"{API}/{PACKAGE}/edits/{self.edit}"


def cmd_status(play, args):
    play.open_edit()
    try:
        tracks = play.call("GET", f"{play.base()}/tracks", mutating=False)
        for t in tracks.get("tracks", []):
            print(f"{t['track']}:")
            for rel in t.get("releases", []):
                codes = ",".join(rel.get("versionCodes", []) or [])
                frac = rel.get("userFraction")
                rollout = f" rollout={frac}" if frac is not None else ""
                print(f"  {rel.get('status'):<12} versionCodes=[{codes}] "
                      f"name={rel.get('name', '-')}{rollout}")
    finally:
        play.abandon()
    return 0


def load_listing():
    """The listing text, with every image it needs checked to exist."""
    try:
        text = play_listing()
    except ListingError as e:
        sys.exit(f"error: {e}")
    missing = [str(p) for files in IMAGES.values() for p in files if not p.exists()]
    if missing:
        sys.exit("missing listing images:\n  " + "\n  ".join(missing))
    return text


def stage_listing(play, text):
    """Write the listing text and every image into the open edit (no commit)."""
    play.call("PUT", f"{play.base()}/listings/{LANG}",
              data=json.dumps({"language": LANG, **text}).encode())
    print(f"  text: title={len(text['title'])}ch short={len(text['shortDescription'])}ch "
          f"full={len(text['fullDescription'])}ch")

    for slot, files in IMAGES.items():
        # deleteall first: uploading alone appends, so re-running would stack
        # duplicate screenshots up against Play's 8-per-slot ceiling.
        play.call("DELETE", f"{play.base()}/listings/{LANG}/{slot}")
        for f in files:
            ctype = mimetypes.guess_type(f.name)[0] or "image/png"
            play.call("POST",
                      f"{UPLOAD}/{PACKAGE}/edits/{play.edit}/listings/{LANG}/{slot}"
                      f"?uploadType=media",
                      data=f.read_bytes(), content_type=ctype)
        print(f"  {slot}: {len(files)} image(s)")


def cmd_listing(play, args):
    text = load_listing()
    play.open_edit()
    print(f"edit {play.edit}")
    try:
        stage_listing(play, text)
        play.commit()
    except BaseException:
        play.abandon()
        raise
    return 0


def cmd_release(play, args):
    tracks = list(dict.fromkeys(args.track))  # de-duplicated, order kept
    if "production" in tracks and args.rollout is None:
        print("note: no --rollout given, so this goes to 100% of production users",
              file=sys.stderr)
    notes = args.notes or DEFAULT_NOTES
    # Read before the edit opens, so a bad listing fails without touching Play.
    text = load_listing() if args.with_listing else None

    def release_for(track):
        release = {
            "versionCodes": [str(args.build)],
            "name": f"release-{args.build}",
            "releaseNotes": [{"language": LANG, "text": notes}],
        }
        # A rollout fraction is about limiting a PUBLIC audience; internal
        # testers always get the whole build.
        if args.rollout is not None and track != "internal":
            release["status"] = "inProgress"
            release["userFraction"] = args.rollout
        else:
            release["status"] = "completed"
        return release

    play.open_edit()
    print(f"edit {play.edit}")
    try:
        if args.upload:
            aab = Path(args.upload)
            r = play.call("POST",
                          f"{UPLOAD}/{PACKAGE}/edits/{play.edit}/bundles?uploadType=media",
                          data=aab.read_bytes(), content_type="application/octet-stream")
            # The versionCode is baked into the bundle by the Makefile. If it
            # disagrees with --build, the tracks below would name a build that is
            # not the one just uploaded.
            if not play.dry_run and r.get("versionCode") != args.build:
                sys.exit(f"{aab.name} is versionCode {r.get('versionCode')}, "
                         f"not {args.build}")
            print(f"  uploaded {aab.name} ({aab.stat().st_size} bytes)")
        elif not play.dry_run:
            # Fail early and clearly if that versionCode was never uploaded,
            # rather than letting the commit fail with a less obvious message.
            bundles = play.call("GET", f"{play.base()}/bundles", mutating=False)
            codes = {b["versionCode"] for b in bundles.get("bundles", [])}
            if args.build not in codes:
                sys.exit(f"versionCode {args.build} is not uploaded "
                         f"(uploaded: {sorted(codes)}); the release workflow uploads it "
                         f"to the internal track on every merge to main")

        if text is not None:
            stage_listing(play, text)

        for track in tracks:
            release = release_for(track)
            play.call("PUT", f"{play.base()}/tracks/{track}",
                      data=json.dumps({"track": track, "releases": [release]}).encode())
            print(f"  track {track}: versionCode {args.build}, status {release['status']}"
                  + (f", rollout {release['userFraction']}" if "userFraction" in release
                     else ""))
        # Committing an edit is what submits it for review, so there is no
        # separate submit step to gate.
        play.commit(skip_if_busy=args.skip_if_busy)
    except BaseException:
        play.abandon()
        raise
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="verb", required=True)

    sub.add_parser("status", help="what is on each track")

    p_listing = sub.add_parser("listing", help="push listing text + images")
    p_listing.add_argument("--dry-run", action="store_true")

    p_rel = sub.add_parser("release", help="put a build on one or more tracks")
    p_rel.add_argument("--build", type=int, required=True, help="release number = versionCode")
    p_rel.add_argument("--track", required=True, action="append", choices=TRACKS,
                       help="track to release onto; repeat for several, all in one edit")
    p_rel.add_argument("--upload", metavar="AAB",
                       help="upload this bundle in the same edit first")
    p_rel.add_argument("--with-listing", action="store_true",
                       help="push listing text + images in the same edit")
    p_rel.add_argument("--rollout", type=float,
                       help="staged rollout fraction 0-1 for non-internal tracks; "
                            "omit for a full release")
    p_rel.add_argument("--notes", help=f"release notes (default: {DEFAULT_NOTES!r})")
    p_rel.add_argument("--skip-if-busy", action="store_true",
                       help="exit 0 instead of cancelling changes already in review")
    p_rel.add_argument("--dry-run", action="store_true")

    args = ap.parse_args()
    rollout = getattr(args, "rollout", None)
    if rollout is not None and not 0 < rollout <= 1:
        sys.exit("--rollout must be in (0, 1]")

    play = Play(dry_run=getattr(args, "dry_run", False))
    return {"status": cmd_status, "listing": cmd_listing, "release": cmd_release}[args.verb](
        play, args)


if __name__ == "__main__":
    sys.exit(main())
