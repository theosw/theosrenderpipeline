# Weather and time presets

Presets live in the **Neural Rendering** tab. A preset changes any Neural
Rendering setting or sharpening for the weather or places you choose, using
Skyrim's weather and game clock. ENB Helper is not required. Up to **512 named
presets** and **4096 exact weather assignments** are supported.

## Layout

The left column lists **Base** first, then your presets. Base is your normal
Neural Rendering settings. Its controls list each label on the left and its
control on the right; per-pass settings sit in a table with Pass 1 and Pass 2 side
by side, followed by Sharpening, Performance and Reconstruction. Settings that do
not apply are greyed out, never hidden. Selecting a preset shows the same controls
with its changes marked. Each preset row says when it applies and how many settings it
changes, and **NOW** marks the rows your edited settings select for the current
weather. Once a preset exists, a **Now** line shows what is applied, the game time
and weather; hover it for the resolved values.

Presets apply whenever at least one preset is in use. There is no separate
automation switch. **Pause presets** immediately uses Base until you untick it or
restart the game; it is not saved.

## Create and edit presets

1. Tune Base. Sharpening applies with or without NR. The **Pass 2** checkbox in the
   pass table header turns the second pass on. Pass 2 follows Pass 1 until you
   change one of its settings, which gives it its own settings starting from Pass
   1's; **Match Pass 1** makes it follow Pass 1 again.
2. **Create new preset** starts with no changes. **Duplicate** copies the selected
   preset.
3. Under **Use when**, tick weather types (Clear, Cloudy, Rain, Snow), Interior or
   Outdoors, and add specific weathers. These controls wrap to fit the window.
4. Change the settings you want different. A changed setting is amber, shows Base's
   value, and has **Reset**; setting it back to Base's value also removes the
   change. **Reset all to Base** clears every change. Settings you leave alone
   follow Base, including later Base edits. In the pass table each pass's cell is
   marked separately, so a preset can change only Pass 2's style.
5. **Apply** for this session or **Save as default** for future sessions.
   **Discard** restores the applied configuration.

Look settings (intensity, local tone, local structure and sharpening strength)
blend smoothly between weathers and times of day and keep NR history. With
**Different look by time of day** (under Use when) on, they can differ at each of
the six times; pick the time to edit, and the current time is green. Turning it
off keeps the selected time's values. Other settings switch once at the midpoint
of a weather change and reset NR history.

Settings marked **!** restart NR briefly when they change: placement, NR input
resolution, network preset, peripheral compression, combined preparation and
Reconstruction. A preset that changes one of them can hitch when the weather
changes; interior and exterior changes happen behind a loading screen. Pass count
switches without restarting NR: two passes stay allocated while any preset uses
them, and one-pass situations skip Pass 2 like the combat option.

Presets never switch NR or sharpening on while Base has them off, but can switch
them off. The Image tab shows Base sharpening read-only, and the value in use when
a preset changes it. Community Shaders owns sharpening on its route, so presets
change only TRP NR there. **Use this preset** in the preset's header ignores it without
deleting it. **Timing...** holds the times of day and transition
smoothing. See the [focused game check](APPEARANCE-TESTING.md) before relying on a
new preset set.

## Weather types and specific weathers

Several presets can use the same weather type, Interior, Outdoors or specific
weather; the one highest in the list wins, and **Move up**/**Move down** change the
order. A claim that a higher preset wins is shown muted, naming that preset.
Specific weathers show as chips; click a chip to remove it.
**Add current weather** is a shortcut for the incoming weather.

**Add weathers...** searches editor ID/name, owning plugin, FormID and weather
type. Names use the engine or the optional Native EditorID Fix and powerofthree's
Tweaks public lookup APIs when loaded. Neither plugin is required; a plugin/local
FormID label remains available otherwise. **Refresh list** resamples names on
Skyrim's main thread. The list includes loaded records, not just weathers that
normally occur in the current region. Select rows individually or use **Select
shown** after filtering; selection survives filtering. The list shows weathers
another preset already uses. Capacity errors leave the batch unchanged.

Assignments for absent plugins remain saved as greyed chips; they do not match
another plugin with the same numeric ID. Deleting a preset removes its assignments;
its weathers fall to the next preset that uses them, or to less specific ones.

Outdoor resolution is Base, then Outdoors, then Clear/Cloudy/Rain/Snow, then a
specific weather, per setting: a more specific preset overrides only the settings
it changes. The Now line names every preset in use, most specific first. Fog, ash and
special-worldspace weather can be added as specific weathers; their visible
appearance is not reliably described by the game's broad classification alone.
Ordinary interiors use only Interior over Base. Sky-lit interior cells follow
outdoor rules.

## Time and transitions

Default anchors are Night 00:00, Dawn 05:00, Sunrise 07:00, Day 12:00, Sunset 18:00
and Dusk 20:00. Values interpolate between adjacent anchors, including midnight.
Hours must be within a 24-hour day and increase in that order.

**Copy ENB timing as anchors** reads `enbseries.ini` beside the game executable
only when clicked. It copies NightTime, SunriseTime, DayTime and SunsetTime and
derives Dawn as SunriseTime minus DawnDuration, Dusk as SunsetTime plus DuskDuration.
For example, markers 1/8.5/10.5/17.5 with durations 3.5/4 give anchors
01:00/05:00/08:30/10:30/17:30/21:30. Review and Apply the result. Missing or
non-increasing schedules leave the previous anchors unchanged.

This provides an editable approximation from the ENB configuration. TRP retains
its own linear interpolation; it does not reproduce ENB's internal phase weights,
follow later ENB edits automatically or change ENB settings.

Both outgoing and incoming presets are sampled at the current hour, then blended
using Skyrim's weather transition progress. Additional smoothing softens sudden
changes. Its seconds value is an exponential time constant, not a fixed completion
duration. Loading clears the old scene's blend. Zero smoothing follows the
weather/time result directly. Base values and authored presets never receive
intermediate blended values.

## Preset files and sharing

Each preset is one file in `Data/SKSE/Plugins/TheosRenderPipeline/Presets/`, and
the file name is its name: `Moody rain.ini` is the preset "Moody rain". Renaming the
file renames the preset, and a copied file is a new preset. Files added to that
folder load at the next game start. Names cannot contain characters Windows file
names forbid, such as `?` or `:`. Renaming a preset in game renames its file; under
MO2 the renamed file is written to Overwrite.

Under Mod Organizer, presets you create are written to Overwrite, like other
settings the game creates; saving a preset that came from a mod updates that mod's
file. To package presets as a mod:

1. In MO2, choose **Create empty mod** and name it, for example
   `TRP Presets - Moody Weather`.
2. Open the new mod's folder and create `SKSE/Plugins/TheosRenderPipeline/Presets/`.
3. Copy the preset `.ini` files you want from MO2's Overwrite (the same path) into
   that folder, and enable the mod.

Right-clicking Overwrite and choosing **Create mod...** does the same in one step
for everything Overwrite contains. The mod is a preset pack: upload it as an
ordinary Data mod, and players or modlists install and enable it like any other
mod. When two mods contain the same preset file, MO2's mod order decides which one
the game uses.

A preset stores only the settings it changes, so it looks different on a
different Base. **Save full copy** adds a copy with every setting stored, which
looks the same for anyone; use it for packs meant to be a complete look, with
Outdoors and Interior ticked. A preset file contains:

```ini
[Preset]
Format = 1
Enabled = true
UseWhen = Rain|Snow
WeatherCount = 1
Weather0 = Skyrim.esm|10A241

[Changes]
Pass2Style = 7

[Changes.Night]
Pass1Intensity = 1.2
```

`UseWhen` accepts Outdoors, Clear, Cloudy, Rain, Snow and Interior. Weathers are a
plugin name and its local FormID, so they work in any load order. `[Changes]` uses
the same keys as the Neural Rendering settings; look settings go in the six time
sections (Night, Dawn, Sunrise, Day, Sunset, Dusk).

## Persistence and compatibility

`Data/SKSE/Plugins/TheosRenderPipeline.ini` keeps `[Appearance]` format 4: the time
schedule, smoothing and `PresetOrder`, the list order by file name. Files it does
not list, such as newly added packs, follow in name order. `Enabled` is written as
whether any preset is in use, for older builds; this build derives it on load.

Save as default writes every preset file, renames the file of a renamed preset and
deletes the files of deleted presets; Discard undoes both before saving. Preset
files the game has not loaded are left alone. Presets
from formats 1-3 in the main INI load as before and move into files at the next
save, which removes their old sections. Unrelated INI settings remain intact.
Older plugin builds cannot read format 4 or preset files; keep a backup before
reverting the DLL.

Install the matching `RCAS.hlsl` with the DLL. Sharpening uses a runtime constant
buffer; unchanged strength does not upload it again. Weather identities and preset
selection are cached across ordinary frames. Disabled automation samples the
context display at four updates per second.

Gradual look changes retain NR history. Manual/preset edits, loading, camera
resets, time jumps and other setting changes still reset it. Standalone tests
do not establish NVIDIA temporal image quality, game performance or visual
acceptance of the new UI.
