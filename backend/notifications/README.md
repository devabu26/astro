# Astro push gateway

`cloudflare-worker.js` is the Cloudflare Worker source for Astro's Matrix HTTP pusher. It forwards Matrix push-gateway requests to the Ubuntu Touch push service.

## Deploy the Worker

The currently deployed `astro-push-gateway` endpoint was previously only a scaffold: it acknowledged Matrix requests with `{"rejected":[]}` but did not forward them to Ubuntu Push. Deploy the source in `cloudflare-worker.js` to make delivery real.

1. Open Cloudflare Dashboard → Workers & Pages → `astro-push-gateway` → Edit code.
2. Replace the Worker source with the contents of `cloudflare-worker.js` and deploy.
3. Create a Workers KV namespace for push deduplication.
4. Bind it to this Worker using the variable name `ASTRO_PUSH_DEDUP`NaN`https://push.ubuntu.com/notify` using the Ubuntu Push API. It uses the app ID `astro.madabu_astro`, which must match `PushClient.appId` and the Matrix pusher app ID in Astro.

## What it sends

- Incoming MSC4075 ring events (`org.matrix.msc4075.rtc.notification` and `m.rtc.notification`) become persistent Ubuntu notifications with a click-through `astro://open?kind=call` URI.
- Other Matrix push events become ordinary, non-persistent message notifications.
- The Matrix pusher's `privacy_level` setting controls whether message bodies are included in ordinary message notification cards.
- Calls use the Matrix ring's `sender_ts` and `lifetime` to set the upstream push expiry and to let Astro reject stale call alerts.
- KV deduplication suppresses duplicate Matrix push-gateway requests for the same event and pushkey.

## Test

After deploying, register or refresh Astro's pusher by launching Astro and signing into Matrix. Check the app logs for:

- `ASTRO PUSH: installed astro_rtc_ring_msc4075 push rule` (or `already enabled`)
- `ASTRO PUSH: installed astro_rtc_ring_stable push rule` (or `already enabled`)
- `ASTRO: Matrix push pusher registered`

Then place a call to the Astro account while Astro is backgrounded or closed. The Worker should forward the incoming ring to Ubuntu Push and the device should display a persistent notification. Tapping it launches Astro and opens the incoming-call banner; it does not auto-accept the call.

## Known lifecycle limitation

Ubuntu Push supports persistent notification cards and the app can clear them using `PushClient.clearPersistent()`. Astro clears the call card when the app receives the ring, when the user dismisses/answers/declines, or when a queued push is found to be expired on a later app activation. If Astro remains completely closed after the remote caller hangs up, the already displayed persistent notification cannot currently be removed remotely by this Worker; it is cleared the next time Astro processes its push queue.
