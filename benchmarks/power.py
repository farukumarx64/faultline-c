"""Versioned macOS power conditions. Read-only: never changes OS settings."""

import json
from pathlib import Path
import re
import time

from run_batch import require

PROFILE = Path(__file__).resolve().parent / 'profiles/power-v1.json'
NAMES = ('ac', 'battery', 'battery-low-power')


def load_profile(name='ac'):
    document = json.loads(PROFILE.read_text())
    require(document.get('contract') == 'faultline-power-v1' and
            document.get('observation_policy') == 'before_and_after_each_sample',
            'unsupported power contract')
    expected = {
        'ac': dict(id='ac-normal-v1', source='AC', low_power_mode=False,
                   initial_charge_range_percent=None, minimum_charge_percent=None),
        'battery': dict(id='battery-normal-v1', source='Battery', low_power_mode=False,
                        initial_charge_range_percent=[90, 100], minimum_charge_percent=20),
        'battery-low-power': dict(id='battery-low-power-v1', source='Battery', low_power_mode=True,
                                  initial_charge_range_percent=[90, 100], minimum_charge_percent=20)}
    require(document.get('profiles') == expected and name in expected, 'unsupported power profile')
    return dict(contract=document['contract'], name=name,
                observation_policy=document['observation_policy'], **document['profiles'][name])


def source_and_charge(text):
    sources = re.findall(r"^Now drawing from '(AC|Battery) Power'\s*$", text, re.M)
    require(len(sources) == 1, 'power source unavailable or ambiguous')
    batteries = [line for line in text.splitlines() if '-InternalBattery-' in line]
    require(len(batteries) <= 1, 'multiple internal batteries are unsupported')
    charge = None
    if batteries:
        percentages = re.findall(r'(?<!\S)(\d+)%;', batteries[0])
        require(len(percentages) == 1 and 0 <= int(percentages[0]) <= 100,
                'battery charge unavailable or invalid')
        require('present: true' in batteries[0], 'internal battery is not present')
        charge = int(percentages[0])
    require(sources[0] != 'Battery' or charge is not None, 'battery charge is required')
    return sources[0], charge


def low_power_mode(settings, source, *, active=False):
    if active:
        sections = settings.split('Currently in use:', 1)
        require(len(sections) == 2, 'effective power settings unavailable')
        section = sections[1]
    else:
        # Match the selected section only, regardless of section order. An AC
        # setting must never stand in for the battery setting (or vice versa).
        sections = re.findall(r'^([^\s][^\n]*):\s*\n((?:[ \t]+[^\n]*\n?)*)', settings, re.M)
        matching = [body for name, body in sections if name == source + ' Power']
        require(len(matching) == 1, f'{source} power settings unavailable or ambiguous')
        section = matching[0]
    modes = re.findall(r'^\s*lowpowermode\s+(\d+)\s*$', section, re.M)
    require(len(modes) == 1 and modes[0] in ('0', '1'), 'Low Power Mode unavailable or ambiguous')
    return modes[0] == '1'


def validate_thermal(thermal, *, low_power=False):
    warnings = re.findall(r'(?:Thermal_Level|Performance_Warning|CPU_Speed_Limit|'
                          r'CPU_Scheduler_Limit)\s*=\s*(\d+)', thermal)
    for name, value in re.findall(r'(\w+)\s*=\s*(\d+)', thermal):
        if name in ('Thermal_Level', 'Performance_Warning'):
            require(int(value) == 0, 'active thermal/performance warning')
        if name in ('CPU_Speed_Limit', 'CPU_Scheduler_Limit'):
            require(1 <= int(value) <= 100, 'invalid CPU performance limit')
            require(low_power or int(value) == 100, 'CPU performance is limited')
    require(all('warning' not in line.lower() or 'no ' in line.lower() or
                re.fullmatch(r'\s*Performance_Warning\s*=\s*0\s*', line)
                for line in thermal.splitlines()), 'active thermal/performance warning')
    return bool(warnings)


def validate_observation(observation, profile, *, initial=False):
    require(observation['source'] == profile['source'], f'{profile["source"]} power is required')
    require(observation['low_power_mode'] is profile['low_power_mode'],
            f'Low Power Mode must be {"on" if profile["low_power_mode"] else "off"}')
    if profile['source'] == 'Battery':
        charge = observation.get('charge_percent')
        require(type(charge) is int and profile['minimum_charge_percent'] <= charge <= 100,
                f'battery must remain at or above {profile["minimum_charge_percent"]}% at each observation')
        if initial:
            lower, upper = profile['initial_charge_range_percent']
            require(lower <= charge <= upper, f'battery campaign must start at {lower}–{upper}% charge')


def state(battery, settings, thermal, profile=None, *, effective=None):
    profile = profile or load_profile()
    source, charge = source_and_charge(battery)
    mode = low_power_mode(settings, source)
    if effective is not None:
        require(low_power_mode(effective, source, active=True) == mode,
                'effective and selected power settings disagree')
    observation = dict(source=source, low_power_mode=mode, charge_percent=charge,
                       thermal_raw=thermal.strip(),
                       thermal_telemetry_available=validate_thermal(thermal, low_power=mode))
    validate_observation(observation, profile)
    return observation


def interpret_probe(raw, profile):
    before_source, _ = source_and_charge(raw['battery_before'])
    after_source, _ = source_and_charge(raw['battery_after'])
    require(before_source == after_source, 'power source changed during observation')
    state(raw['battery_before'], raw['settings'], raw['thermal'], profile, effective=raw['effective'])
    return state(raw['battery_after'], raw['settings'], raw['thermal'], profile, effective=raw['effective'])


def probe(owner, label, profile):
    """Bracket settings reads with source checks; retain raw evidence even on failure."""
    raw = dict(observed_monotonic_ns=time.monotonic_ns(), profile=profile)
    try:
        for name, argv in (('battery_before', ['pmset', '-g', 'batt']),
                           ('settings', ['pmset', '-g', 'custom']),
                           ('effective', ['pmset', '-g', 'live']),
                           ('thermal', ['pmset', '-g', 'therm']),
                           ('battery_after', ['pmset', '-g', 'batt'])):
            raw[name], _ = owner.command(label + '-' + name, argv, owner.work_deadline)
        result = interpret_probe(raw, profile)
        raw['power'] = result
        return result
    except Exception as error:
        raw['error'] = f'{type(error).__name__}: {error}'
        raise
    finally:
        raw['finished_monotonic_ns'] = time.monotonic_ns()
        owner.save(label + '-power.json', raw)
