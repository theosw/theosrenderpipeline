# Weather and time appearance profiles

The **Appearance** tab can automatically adjust NR intensity, local tone, local
structure and sharpening strength. Automatic appearance starts off. It reads
Skyrim's weather and game clock directly; ENB Helper is not required.

## Set up a profile

1. Open TRP with **End**, then choose **Appearance**.
2. Turn on **Automatic appearance** and select a group: Exterior, Clear, Cloudy,
   Rain, Snow or Interior. **Profile for current weather** creates an override
   for the incoming weather, initially using your edited NR and Image settings.
3. Enable **Use this profile** and choose whether it overrides NR, sharpening,
   or both. **Copy edited NR / Image settings to all times** provides a starting
   point using your pending settings from those tabs.
4. Choose a time point and adjust its values. **Copy this time to all** makes a
   profile constant throughout the day. Both NR passes can have their own values;
   Pass 2 follows Pass 1 while the NR tab's link option is enabled.
5. **Apply** activates the edits for this session. **Save as default** also saves
   them to `TheosRenderPipeline.ini`. **Discard** restores the applied profiles.

**Use manual settings for this session** immediately pauses automatic appearance
and uses the normal NR and Image values. This is useful for tuning a scene before
capturing the edited values into a profile. The pause is not saved.

Profiles do not enable NR or sharpening themselves. Their existing enable
controls still apply. Community Shaders owns its sharpening, so this feature
only changes TRP NR tuning on the CS route. Network preset, style, skin settings,
input size, placement, pass count and frame generation remain manual controls.

## How selection and transitions work

Exterior selection starts from manual defaults, then applies an enabled Exterior
profile, the weather's classification, and finally its exact-weather override.
Each level can override NR and sharpening independently. Disabled profiles fall
back to the less specific values. Modded or unclassified weather uses the same
fallback; fog and ash can use exact-weather profiles. Up to 64 exact-weather
profiles are supported.

Ordinary interiors use only the Interior profile over manual defaults, so the
outdoor weather does not colour a dungeon's tuning. Interior cells marked to use
sky lighting follow the exterior rules.

The six time points are Night 00:00, Dawn 05:00, Sunrise 07:00, Day 12:00,
Sunset 18:00 and Dusk 20:00 by default. Values interpolate between adjacent
points, including across midnight. **Time schedule** changes those hours; they
must stay in increasing order within a 24-hour day. This is TRP's own schedule,
independent of the climate or ENB's time settings.

Both the incoming and outgoing weather profiles are evaluated at the current
time, then blended using Skyrim's weather transition progress. Additional
transition smoothing softens sudden changes such as a forced weather or entering
an interior. Loading clears the previous scene's blend. Setting smoothing to
zero follows the weather/time result directly. The value is a smoothing time
constant, rather than a guaranteed duration for reaching the target exactly.

The left side shows the current context, selected profiles and resolved values.
These can differ from the settings being edited on the right. Saved defaults
and profile points always retain your authored values, not an intermediate blend.

## Files and compatibility

The configuration lives in `[Appearance]`, `[Appearance.Clear]` and similar
sections in `Data/SKSE/Plugins/TheosRenderPipeline.ini`. Each profile has sections
such as `[Appearance.Clear.Day]` for its time points. Exact-weather entries use
`[Appearance.Weather0]` and subsequent numbered sections, with `Plugin` and a
hexadecimal `FormID` local to that plugin. Plugin names are case-insensitive.
This keeps identities independent of load order, including light plugins. A
profile for a missing weather remains saved but does not match another plugin.

Install the matching `RCAS.hlsl` with the renderer DLL: sharpening strength is
now a runtime shader parameter. Existing manual settings and the default-disabled
appearance system retain their normal operation.

Continuous NR tuning preserves history between gradual automated updates.
Manual/profile edits, camera resets, loading, time jumps and discrete option
changes still reset it. Visual acceptance of moving weather/time transitions
with the supplied NR runtime remains pending; offline tests do not establish
ghosting/flicker behavior in game.
