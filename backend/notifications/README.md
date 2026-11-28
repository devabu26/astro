# Astro push gateway

`cloudflare-worker.js` is the Cloudflare Worker source for Astro's Matrix HTTP pusher. It forwards Matrix push-gateway requests to the Ubuntu Touch push service.

## Deploy or update the Worker

1. Open Cloudflare Dashboard → Workers & Pages → `astro-push-gateway` → Edit code.
2. Deploy the complete contents of `cloudflare-worker.js`.
3. For request deduplication and the diagnostics endpoint, create a Workers KV namespace and bind it using the variable name `ASTRO_PUSH_DEDUP`.
4. Confirm the Worker root endpoint reports that the service is online.

The Worker forwards to `https://push.ubports.com/notify` using the Ubuntu Push API. Its app ID is `astro.madabu_astro`, which must match `PushClient.appId` and Astro's Matrix pusher app ID.

## Notification handling

- Incoming MSC4075 RTC ring events (`org.matrix.msc4075.rtc.notification` and `m.rtc.notification`) and legacy call-notify events (`org.matrix.msc4075.call.notify` and `m.call.notify`) become persistent Ubuntu notifications with an `astro://open?kind=call` click-through URI.
- Other Matrix push events become ordinary, non-persistent message notifications.
- The Matrix pusher's `privacy_level` setting controls whether message bodies are included in ordinary message notification cards.
- Call notifications use `sender_ts` and `lifetime` to set the upstream push expiry and allow Astro to reject stale call alerts.
- KV deduplication suppresses duplicate Matrix push-gateway requests for the same event and pushkey.

## Verify delivery

After deploying, launch Astro and sign in to Matrix so it can register or refresh its pusher. Check the device journal for:

- `ASTRO PUSH: installed astro_rtc_ring_msc4075 push rule` (or `already enabled`)
- `ASTRO PUSH: installed astro_rtc_ring_stable push rule` (or `already enabled`)
- `ASTRO: Matrix push pusher registered`

Then place a call to the Astro account while Astro is backgrounded or closed. Check the Worker's `/__diagnostics` endpoint. A successful upstream status confirms that the Worker handed the notification to Ubuntu Push; it does not by itself prove that the device displayed it.

## Known lifecycle limitation

If Astro remains completely closed after the remote caller hangs up, an already displayed persistent notification cannot currently be removed remotely by this Worker. Astro clears it the next time it processes its push queue.
