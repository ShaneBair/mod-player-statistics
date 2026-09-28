# Online Human Players: AzerothCore Roster Contract

Status: Implemented; target-core and in-game verification pending  
Depends on: Playerbots-compatible AzerothCore test-staging branch with `WorldSession::IsHeadless()`
Consumer: `wow-portal/specs/online-players.md`

## Problem

The portal needs a current roster of real people who are logged into the game. AzerothCore's normal online character state does not express whether the active session is controlled by a person or Playerbots. Historical statistics also cannot determine how a character is controlled in its current session.

The module must provide a live, read-only roster sourced from in-memory AzerothCore sessions and filtered using the Playerbots branch's authoritative `WorldSession::IsHeadless()` flag.

## User Outcome

The portal can retrieve all currently online human-controlled characters, including human-controlled GMs, with enough information to display:

- account login;
- character name;
- race;
- class;
- level;
- a friendly zone label.

Random bots and altbots running under bot control never appear.

## Current Behavior

`mod-player-statistics` records append-only gameplay events in the characters database. It tags recorded actors and applicable targets using the session's headless flag, but it has no live-roster command or presence table.

The existing event table is not suitable for online presence because events are historical and a character can be human-controlled in one session and bot-controlled in another.

## Proposed Design

Add a module-owned AzerothCore command:

```text
playerstats online
```

The command runs on the worldserver command path and inspects current in-memory sessions. It must not query `mod_player_stats_events` or use `characters.online` to decide who is human.

The command is available to the worldserver console and SOAP command execution. The handler must reject invocation from an in-game player session even if that account otherwise has command permission. This prevents the machine-readable account roster from becoming an ordinary in-game command.

No new database table or schema migration is required.

## Roster Inclusion Rules

Include one row for every active session for which all of the following are true:

- the session exists;
- the session has a `Player`;
- the player is fully in the world;
- `session->IsHeadless()` is `false`.

Additional rules:

- Human-controlled GMs are included and treated like other players.
- GM mode, GM visibility, security level, faction, and account type do not otherwise affect inclusion.
- Random bots are excluded.
- Altbots are excluded while their session reports `IsHeadless() == true`, even when their account belongs to a real person.
- If one person has multiple human-controlled sessions online, return one row per character session.
- A session at character selection is not included because it has no in-world player.
- Do not infer bot status from account names, account IDs, character names, or historical events.

## Version 1 Command Output

The successful command writes exactly one module-owned payload line in this form:

```text
PLAYERSTATS_ONLINE_V1 {"generatedAt":1787414400,"players":[]}
```

`PLAYERSTATS_ONLINE_V1 `, including its trailing space, is the marker. Everything after the marker on that line is one valid compact JSON object. The marker allows the portal to locate the contract inside a SOAP command response without treating other SOAP or command text as data.

The top-level version 1 payload is:

```json
{
  "generatedAt": 1787414400,
  "players": [
    {
      "accountId": 12,
      "accountLogin": "SHANE",
      "characterGuid": 345,
      "characterName": "Thalgrim",
      "raceId": 3,
      "classId": 2,
      "level": 42,
      "mapId": 0,
      "zoneId": 33,
      "areaId": 117,
      "location": "Stranglethorn Vale"
    }
  ]
}
```

Field contract:

- `generatedAt` is the Unix timestamp in whole UTC seconds when the roster snapshot was produced.
- `accountId` is the unsigned AzerothCore account ID. It is an integration field and the portal must not return it to browsers.
- `accountLogin` is the AzerothCore authentication username. Preserve the value returned by AzerothCore.
- `characterGuid` is the character GUID counter. It is an integration field and the portal must not return it to browsers.
- `characterName` is the current character name.
- `raceId`, `classId`, and `level` are the player's current unsigned numeric values.
- `mapId`, `zoneId`, and `areaId` are the player's current location IDs.
- `location` is a display-ready English label. Prefer the current zone name; fall back to the area name, then the map name, then `Unknown`.

The module owns field meanings and the marker/version. The portal owns translation of WotLK race and class IDs into display labels.

The `players` array order is not part of the contract. Consumers must sort explicitly.

## Serialization Requirements

- Produce valid JSON using an existing safe JSON facility available in the target core/module build, or a small well-tested encoder.
- Correctly escape quotes, backslashes, control characters, and non-ASCII text.
- Emit the payload on one line without additional module messages before or after it.
- Do not emit IP addresses, email addresses, latency, coordinates, security levels, passwords, session tokens, or SOAP credentials.
- An empty human roster is a successful payload with `players: []`; it is not an error.
- If the command cannot create a trustworthy payload, report command failure without emitting the success marker.

## Account Login Resolution

Use the session account ID as the authoritative key. Prefer a public account-name accessor if the exact deployed Playerbots branch provides one. Otherwise resolve the name through AzerothCore's account manager.

Avoid performing one authentication-database lookup per player on every portal poll:

- cache successful account-ID-to-login resolutions for five minutes;
- cache failed resolutions for no more than 30 seconds;
- keep the cache in memory only;
- never include passwords, verifiers, salts, email addresses, or session secrets.

If an account login cannot be resolved, omit that session from the successful roster and log a server-side warning containing only the account ID. Do not emit a partially identified row.

## Location Resolution

- Resolve labels from AzerothCore's loaded DBC/map data rather than adding portal access to the world database.
- Use the worldserver's default DBC locale for this version.
- Prefer a friendly zone name rather than coordinates or a raw numeric ID.
- Preserve the three numeric location IDs in the integration payload for diagnostics and future presentation changes.
- Unknown or special maps must not fail the complete roster; use the documented fallback chain.

## Performance and Concurrency

- Execute roster collection on the normal worldserver command path.
- Iterate the live session/player collection using the locking or world-thread conventions of the exact target core revision.
- Do not persist, heartbeat, or periodically update a presence table.
- Do not scan the event table.
- The expected server is small, but work should remain linear in active sessions and avoid per-bot account lookups by filtering `IsHeadless()` first.
- The portal will cache successful responses, but correctness must not depend on that cache.

## Configuration

No new module configuration is required for version 1. Installing the module and exposing SOAP already represent operator-controlled capabilities. The command remains read-only and console/SOAP-only.

## Error Behavior

- In-game invocation: reject it with a normal command error and no roster payload.
- Serialization failure: no success marker; log a concise server-side error.
- Account lookup failure for one session: log the account ID, omit that session, and return the remaining valid roster.
- Location lookup failure: include the session with `location: "Unknown"` and the numeric IDs.
- No human players: return a successful empty roster.

## Out of Scope

- Historical login/logout events or playtime tracking.
- A database-backed presence table.
- Bot counts or a bot roster.
- Exact coordinates, orientation, instance progress, battleground state, or group membership.
- Portal authentication or authorization.
- Hiding human GMs.
- Changing existing event-table semantics.

## Acceptance Criteria

- `playerstats online` returns one valid `PLAYERSTATS_ONLINE_V1` payload through the worldserver console and SOAP.
- A normal human player appears with correct account, character, race, class, level, and friendly location values.
- A human GM appears under the same rules.
- Random bots and altbots with `IsHeadless() == true` do not appear.
- A character-selection session does not appear.
- Logging out removes the player from the next command response without waiting for a database cleanup job.
- Changing level or zone is reflected in the next command response.
- Zero humans returns a successful empty array.
- Special characters in any resolved display string cannot break JSON.
- The command does not expose prohibited account or network fields.
- No schema migration is introduced.

## Verification Plan

1. Compile the module inside the exact Dad's MMO Lab Playerbots-compatible AzerothCore source tree.
2. Run the command from the worldserver console with no humans online and validate the marker and JSON.
3. Run it through the same SOAP service account used by the portal.
4. Log in one human character and verify all fields against the in-game character.
5. Enable GM mode for a human and verify the row remains present.
6. Bring random bots and an altbot online and verify they remain absent.
7. Change zones and gain a level, then verify the next snapshot changes.
8. Log out and verify immediate removal.
9. Validate the payload with a strict JSON parser.
10. Confirm repeated portal polling does not create an authentication query per human on every request.

## Consumer Compatibility Rule

Any future field removal, rename, type change, marker change, or semantic change requires a new marker version such as `PLAYERSTATS_ONLINE_V2`. Additive fields may be introduced within version 1 only when version 1 consumers are confirmed to ignore unknown fields.
