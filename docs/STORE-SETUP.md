# Store setup: the parts only a human can do

Building and releasing are separate steps:

- **Build** -- every merge of `staging` into `main` runs `build.yml`: it builds
  every platform, tags `release-N`, and publishes the GitHub Release with all
  the archives, including the signed .ipa and .aab. It never talks to either
  store.
- **Release** -- `release.yml` is run by hand and ships a chosen `release-N`
  from its GitHub Release: Google Play first (upload the AAB, set the chosen
  track, push the listing, all in one edit), then the App Store (upload the
  .ipa, TestFlight notes for internal testers, create version 1.0.N, push the
  listing, submit to App Review), then (this repo) the Fly server. It stops at
  the first failure and reads each store back at the end:

  ```sh
  gh workflow run release.yml -f tag=release-36 -f stores=both -f android_track=alpha
  gh run watch
  ```

  If an earlier version is still waiting for or in App Review, the release
  withdraws it and submits the new one, and the log says exactly what it
  withdrew. Temporary store errors are retried three times, then fail.

The listings are pushed **from the repo on every release**. Edit
`LISTING.md` and the screenshots here, not in either console -- console edits
get overwritten.

What follows is what neither store has an API for.

---

## Apple (about 10 minutes)

1. **Check the API key's role.** App Store Connect → Users and Access →
   Integrations → App Store Connect API → Team Keys → key `7VP4W2NMS5`. It must
   be **App Manager** (or Admin). A Developer-role key can upload to TestFlight
   but cannot submit for review. If it is Developer and the role can't be
   edited, generate a new App Manager key and replace the secrets:

   ```sh
   gh secret set ASC_KEY_ID  -b "<new key id>"
   gh secret set ASC_KEY_P8  < ~/.config/openrackem/AuthKey_<new key id>.p8
   ```

2. **Keep automatic release on.** Apple holds one submission per app. A
   release withdraws a version still waiting for or in review, but it cannot
   withdraw one that is approved and held in **Pending Developer Release** --
   that fails the release until you release or reject it. Leave the app set to
   release automatically on approval so that never happens.

3. **App Privacy label** (App Store Connect → the app → App Privacy). It is
   one-time and has no API. Re-check it covers online play: the matchmaking
   server sees players' IP addresses and in-game moves.

4. **Dry run it.** Nothing to install:

   ```sh
   gh workflow run release.yml -f stores=appstore -f dry_run=true
   gh run watch
   ```

   The run is titled `DRY RUN`; it prints every call it would make and the
   current version state, and ships nothing.

---

## Google Play

Do these in order -- step 6 must come after step 4, or the first automated
upload will hit an app that doesn't exist yet.

### 1. Upload key and the four signing secrets (10 min)

Follow [android/play-assets/KEYSTORE.md](../android/play-assets/KEYSTORE.md)
steps 1-2. Keep the `.jks` and its passwords in `~/.config/openrackem/` next to
the Apple material. The next release after this produces
`openrackem-<N>-android.aab` on the GitHub release.

### 2. Create the app (5 min)

Play Console → **Create app**: name `openrackem`, default language English
(United States), **Game**, **Free**, accept the declarations. (Free is
permanent -- a free app can never become paid.)

### 3. Dashboard "Set up your app" tasks (30-45 min, one time)

The answers are drafted in
[android/play-assets/LISTING.md](../android/play-assets/LISTING.md):

- **Privacy policy:** https://danheskett.com/app/privacy-policy/
- **App access:** all functionality available without special access
- **Ads:** no ads
- **Content rating:** the IARC questionnaire (answer "no" throughout)
- **Target audience:** pick 13+ unless you want the Families policy
  requirements that come with younger ages
- **Data safety:** re-check -- LISTING.md notes why the old "None" answer
  needs a fresh look now that online play exists
- **Government / financial / health / news:** no
- **Store settings:** category Card, email dan@danheskett.com, website
  https://danheskett.com

Leave the store listing text and graphics empty -- the pipeline fills them.

### 4. The first AAB, by hand (10 min)

Play won't take an API upload for an app that has never had one.

```sh
gh release download --repo dannyheskett/openrackem --pattern '*-android.aab'
```

Testing → **Internal testing** → Create new release → accept **Play App
Signing** (Google-generated key) → upload that `.aab` → Save → **Start rollout
to Internal testing**. On the Testers tab, add an email list with yourself.

### 5. Leave managed publishing OFF

Publishing overview → Managed publishing. With it on, approved changes wait
for a console click, and there is no API for that click.

### 6. Service account (15 min)

Reuse openblocks' publisher account -- it is one Play developer account:

1. Google Cloud Console → IAM & Admin → Service accounts → the account
   openblocks publishes with → Keys → **Add key → JSON**. (Its old key file isn't
   in `~/.config/openblocks/`, so mint a new one.)
2. Play Console → **Users and permissions** → that account → **App permissions**
   → add `openrackem` with: *Release to production*, *Release apps to testing
   tracks*, *Manage testing tracks and edit tester lists*, *Manage store
   presence*.
3. Store the key and set the secret:

   ```sh
   mv ~/Downloads/<key>.json ~/.config/openrackem/play-publisher.json
   chmod 600 ~/.config/openrackem/play-publisher.json
   gh secret set PLAY_SERVICE_ACCOUNT_JSON < ~/.config/openrackem/play-publisher.json
   ```

4. Check it: `gh workflow run release.yml -f stores=play -f dry_run=true`, or
   locally `python3 scripts/play_release.py status` (it reads that same file).

From then on, `release.yml` puts a build on whichever track you choose.

### 7. Closed testing -- the 14-day gate

New personal developer accounts must run a closed test with **at least 12
testers opted in for 14 consecutive days** before production access. (The
Dashboard says whether this applies to the account.)

1. Create a Google Group and add 12+ testers. The API manages testers only as
   Google Groups, never email lists.
2. Testing → **Closed testing** → the default track (the API calls it `alpha`)
   → Testers → add the group. Share the opt-in link; testers must accept it
   and stay opted in.
3. Release to it: `gh workflow run release.yml -f android_track=alpha`.

### 8. Production

After the 14 days: Dashboard → **Apply for production** (a short
questionnaire; Google's review takes several days). Once granted:

```sh
gh workflow run release.yml -f android_track=production -f rollout=0.2   # staged 20%
```

---

## First-run watch points

Both store paths were exercised against a fake server, not the live stores.
The first real run is the test; check these in its log:

- **Play "in review" skip.** When Play has changes in review, `publish-play`
  should print `Play already has changes in review ... skipping` and pass. It
  recognises the refusal by the words "in review" in Play's error. If Play
  words it differently, the job goes red with the full message -- send it over
  and the match gets adjusted.
- **"Only releases with status draft may be created on draft app".** Play still
  considers the app a draft: finish the Dashboard tasks in step 3 and the
  step 4 rollout.
- **App Store** -- the same code has been shipping openblocks to review since
  2026-09-10.
