"""Simulates game seeds from the current second on and prints the games that drop the items you want.

Run it through drops.ps1, which prepares the simulator; drops.ps1 --help shows this help.
The workers send their items straight to this script, so a search can run for hours without using disk.
"""

import argparse
import csv
import datetime
import os
import queue
import signal
import subprocess
import sys
import threading
import time

DIFFICULTIES = ['Normal', 'Nightmare', 'Hell']
BELOW_NORMAL_PRIORITY_CLASS = 0x4000
CREATE_NO_WINDOW = 0x08000000

ITEM_HEADER = [
    'game_seed', 'difficulty', 'dlvl', 'set_level', 'source_kind', 'source_name', 'source_index',
    'item_type', 'base_item', 'item_level', 'quality',
    'prefix', 'prefix_text', 'prefix_value', 'prefix_value2',
    'suffix', 'suffix_text', 'suffix_value', 'suffix_value2',
    'unique_name', 'spell', 'charges', 'min_dam', 'max_dam', 'ac', 'max_dur', 'req_str', 'req_mag', 'req_dex',
    'item_value', 'name', 'idx', 'iseed', 'create_info', 'prefix_roll', 'suffix_roll',
    'source_x', 'source_y',
]


HELP_DESCRIPTION = """\
Searches game seeds from right now on and prints each game that drops an item on your wishlist,
with a code to paste into the patched game's Enter Game Seed box. Runs until Ctrl+C unless
--minutes is given; Ctrl+C prints a summary.
"""

HELP_EXAMPLES = """\
examples:
  drops.ps1 --type ring amulet --prefix Obsidian Gold "Dragon's" --suffix life "the zodiac" --min-roll 80
  drops.ps1 --base Maul "Great Axe" --prefix "King's" --suffix haste --difficulty hell --minutes 30
  drops.ps1 --unique "Harlequin Crest" --workers 10

Names are not case-sensitive; quote names with spaces or apostrophes. "of " in suffixes is optional.
"""


class HelpFormatter(argparse.RawDescriptionHelpFormatter):
    """Shows a list option as "--type NAME ..." instead of "--type TYPE [TYPE ...]"."""

    def _format_args(self, action, default_metavar):
        if action.nargs == '+':
            return f'{self._metavar_formatter(action, default_metavar)(1)[0]} ...'
        return super()._format_args(action, default_metavar)


def parse_args():
    parser = argparse.ArgumentParser(prog='drops.ps1', usage='drops.ps1 <item requirements> [game settings] [search options]',
                                     description=HELP_DESCRIPTION, epilog=HELP_EXAMPLES, add_help=False,
                                     formatter_class=lambda prog: HelpFormatter(prog, max_help_position=30))
    parser.add_argument('--bin', required=True, help=argparse.SUPPRESS)

    wishlist = parser.add_argument_group('item requirements')
    wishlist.add_argument('--type', nargs='+', default=[], metavar='TYPE',
                          help='ring amulet sword axe mace bow staff helm shield light_armor medium_armor heavy_armor')
    wishlist.add_argument('--base', nargs='+', default=[], metavar='NAME', help='base items, e.g. Maul "Great Axe"')
    wishlist.add_argument('--prefix', nargs='+', default=[], metavar='NAME', help='the item\'s prefix must be one of these')
    wishlist.add_argument('--suffix', nargs='+', default=[], metavar='NAME', help='the item\'s suffix must be one of these')
    wishlist.add_argument('--either', action='store_true', help='with both lists, one matching affix is enough')
    wishlist.add_argument('--unique', nargs='+', default=[], metavar='NAME', help='unique items, e.g. "Harlequin Crest"')
    wishlist.add_argument('--min-roll', type=int, metavar='PERCENT',
                          help='each wanted affix rolled at least this far up its range; 80 is the top fifth')
    wishlist.description = 'A prefix or suffix can carry its own minimum for the first number it shows: Obsidian:38.'

    games = parser.add_argument_group('game settings', 'The host has to create the game with these settings.')
    games.add_argument('--difficulty', nargs='+', default=[], metavar='LEVEL', help='normal nightmare hell, or 1 2 3 (default: all three)')
    games.add_argument('--full-quests', choices=['on', 'off', '1', '0'], default='on', metavar='on|off',
                       help='Full quests in Multiplayer (default on)')
    games.add_argument('--randomize-quests', choices=['on', 'off', '1', '0'], default='on', metavar='on|off',
                       help='Randomize Quests (default on)')

    running = parser.add_argument_group('search')
    running.add_argument('--minutes', type=float, default=0, metavar='N', help='stop after this long (default: until Ctrl+C)')
    running.add_argument('--workers', type=int, default=20, metavar='N', help='simulation processes; fewer leaves more CPU for playing (default 20)')
    running.add_argument('--start', type=int, metavar='SEED', help='first seed (default: the current second)')
    running.add_argument('--help', action='help', help='show this help')

    args = parser.parse_args()
    args.full_quests = args.full_quests in ('on', '1')
    args.randomize_quests = args.randomize_quests in ('on', '1')
    difficulties = {'normal': 0, 'nightmare': 1, 'hell': 2, '1': 0, '2': 1, '3': 2}
    for name in args.difficulty:
        if name.lower() not in difficulties:
            parser.error(f'unknown difficulty "{name}": use normal, nightmare, hell, or 1, 2, 3')
    args.difficulty = {difficulties[name.lower()] for name in args.difficulty}
    if not (args.type or args.base or args.prefix or args.suffix or args.unique):
        parser.error('give at least one of --type, --base, --prefix, --suffix, --unique')
    if args.workers < 1:
        parser.error('--workers must be at least 1')
    return args


def creation_time(game_seed):
    """The game seed is the host's clock (seconds since 1970 UTC) when the game is created."""
    return datetime.datetime.fromtimestamp(game_seed).strftime('%Y-%m-%d %I:%M:%S %p')


def split_minimum(spec):
    """"Obsidian:38" -> ("Obsidian", 38): the first number the affix shows in game must be at least 38."""
    name, sep, minimum = spec.rpartition(':')
    if sep and minimum.isdigit():
        return name, int(minimum)
    return spec, None


def check_names(args):
    """Stops on a name the game doesn't have, before any time is spent simulating."""
    known = {}
    with open(os.path.join(args.bin, 'names.csv'), encoding='utf-8', newline='') as f:
        for row in csv.DictReader(f):
            known.setdefault(row['kind'], set()).add(row['name'].lower())
    known['type'] = {'ring', 'amulet', 'sword', 'axe', 'mace', 'bow', 'staff', 'helm', 'shield', 'light_armor', 'medium_armor', 'heavy_armor'}
    for kind, names in (('type', args.type), ('base', args.base), ('prefix', args.prefix), ('suffix', args.suffix), ('unique', args.unique)):
        for name in names:
            name = split_minimum(name)[0].lower()
            if kind == 'suffix' and name.startswith('of '):
                name = name[3:]
            if name not in known[kind]:
                close = sorted(n for n in known[kind] if n[:3] == name[:3])[:8]
                sys.exit(f'the game has no {kind} called "{name}"' + (f'; similar: {", ".join(close)}' if close else ''))


class Wishlist:
    def __init__(self, args):
        lower = lambda names: [n.lower() for n in names]
        self.types = set(lower(args.type))
        self.bases = set(lower(args.base))
        self.prefixes = dict(split_minimum(p) for p in lower(args.prefix))
        self.suffixes = {}
        for spec in lower(args.suffix):
            name, minimum = split_minimum(spec)
            self.suffixes[name[3:] if name.startswith('of ') else name] = minimum
        self.uniques = set(lower(args.unique))
        self.either = args.either
        self.difficulties = args.difficulty
        self.min_roll = args.min_roll

    def matches(self, row):
        if self.difficulties and int(row['difficulty']) not in self.difficulties:
            return False
        if self.types and row['item_type'] not in self.types:
            return False
        if self.bases and row['base_item'].lower() not in self.bases:
            return False
        if self.uniques and row['unique_name'].lower() not in self.uniques:
            return False
        has_prefix = self.affix_ok(self.prefixes, row['prefix'], row['prefix_value']) and self.roll_ok(row['prefix_roll'])
        has_suffix = self.affix_ok(self.suffixes, row['suffix'], row['suffix_value']) and self.roll_ok(row['suffix_roll'])
        if self.prefixes and self.suffixes:
            if not ((has_prefix or has_suffix) if self.either else (has_prefix and has_suffix)):
                return False
        elif self.prefixes and not has_prefix:
            return False
        elif self.suffixes and not has_suffix:
            return False
        return True

    def roll_ok(self, roll):
        return self.min_roll is None or (roll != '' and int(roll) >= self.min_roll)

    @staticmethod
    def affix_ok(wanted, name, value):
        name = name.lower()
        if name not in wanted:
            return False
        minimum = wanted[name]
        return minimum is None or (value != '' and int(value) >= minimum)


class Worker:
    """One simulation process. Its rows arrive on stdout: item rows start with "I,", game rows with "G,"."""

    def __init__(self, index, args, first_seed, stop_at, lines, hung):
        self.index = index
        self.args = args
        self.hung = hung
        self.next_seed = first_seed + index
        self.stop_at = stop_at
        self.lines = lines
        self.restarts = 0
        self.finished = False
        self.process = None
        self.start()

    def start(self):
        env = dict(os.environ, DROPSTATS_WORKER=str(self.index), DROPSTATS_FULL_QUESTS='1' if self.args.full_quests else '0',
                   DROPSTATS_RANDOMIZE_QUESTS='1' if self.args.randomize_quests else '0',
                   DROPSTATS_FIRST_SEED=str(self.next_seed), DROPSTATS_SEED_STEP=str(self.args.workers),
                   DROPSTATS_SKIP_LEVELS=';'.join(f'{seed}:{dlvl}:{set_level}' for seed, dlvl, set_level in self.hung if seed == self.next_seed))
        if self.stop_at is not None:
            env['DROPSTATS_STOP_AT'] = str(self.stop_at)
        self.process = subprocess.Popen([os.path.join(self.args.bin, 'drop_stats_test.exe'), '--gtest_filter=DropStats.SearchWorker'],
                                        cwd=self.args.bin, env=env, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                        text=True, encoding='utf-8', errors='replace',
                                        creationflags=BELOW_NORMAL_PRIORITY_CLASS | CREATE_NO_WINDOW)
        threading.Thread(target=self.read, args=(self.process,), daemon=True).start()

    def read(self, process):
        for line in process.stdout:
            self.lines.put((self, line))
        self.lines.put((self, None))

    def ended(self):
        """Called when the process's output ends. Returns True if it was restarted after a level that hangs."""
        self.process.wait()
        if self.finished:
            return False
        if self.restarts >= 20:
            print(f'worker {self.index} keeps stopping early, giving up on it')
            return False
        # The worker reported the level that hung; the restarted one skips it on the same seed.
        self.restarts += 1
        self.start()
        return True


SET_LEVEL_NUMBERS = {"King Leoric's Tomb": 1, 'Chamber of Bone': 2, 'Poisoned Water Supply': 4, "Lazarus' Lair": 5}


def seed_code(item):
    """What to type in the patched game's Enter Game Seed box: the seed, then the level and the item's source to
    mark on the automap. Monsters and objects keep the numbers they get when the level is generated, so the
    game can follow the exact monster even when it moves."""
    level = f"s{SET_LEVEL_NUMBERS[item['set_level']]}" if item['set_level'] else item['dlvl']
    if item['source_index'] == '-1':
        target = f"{item['source_x']},{item['source_y']}"
    else:
        target = f"{'m' if item['source_kind'] in ('monster', 'unique_monster') else 'o'}{item['source_index']}"
    return f"{item['game_seed']}-{level}:{target}"


def short_time(game_seed):
    """The game's creation time, with the date only when it isn't today."""
    created = datetime.datetime.fromtimestamp(game_seed)
    pattern = '%I:%M %p' if created.date() == datetime.date.today() else '%b %d %I:%M %p'
    return created.strftime(pattern).replace(' 0', ' ').lstrip('0')


def same_drop(item):
    """Items that are the same drop on different difficulties: same item, same level, same source."""
    return tuple(item[key] for key in ('name', 'prefix_text', 'suffix_text', 'dlvl', 'set_level', 'source_index', 'source_x', 'source_y'))


def print_drop(drop):
    """Prints one drop like a log entry: the item flush left, the rest indented."""
    item = drop['item']
    powers = ', '.join(t for t in (item['prefix_text'], item['suffix_text']) if t)
    where = item['set_level'] or f"dlvl {item['dlvl']}"
    difficulties = ', '.join(DIFFICULTIES[d] for d in sorted(drop['difficulties']))
    print(f"{item['name']}   {powers}" if powers else item['name'])
    print(f"  {seed_code(item)}   {where}, {item['source_name']} at {item['source_x']},{item['source_y']}")
    print(f"  {difficulties}   {short_time(int(item['game_seed']))}")
    if drop['hung']:
        print(f"  The game hangs entering {drop['hung']}; stay out of it.")


def main():
    args = parse_args()
    check_names(args)
    wishlist = Wishlist(args)
    first_seed = args.start if args.start is not None else int(time.time())
    stop_at = int(time.time() + args.minutes * 60) if args.minutes > 0 else None
    # Ctrl+Break stops the search the same way Ctrl+C does.
    signal.signal(signal.SIGBREAK, signal.default_int_handler)

    on_off = lambda value: 'ON' if value else 'OFF'
    how_long = f'{args.minutes:g} minutes' if stop_at else 'until Ctrl+C'
    print('Search   (host the game with these settings)')
    print(f'  Full quests in Multiplayer   {on_off(args.full_quests)}')
    print(f'  Randomize Quests             {on_off(args.randomize_quests)}')
    print(f'  Games created from           {short_time(first_seed)}')
    print(f'  Running                      {how_long} on {args.workers} workers')
    print()
    print('Ctrl+C stops early.')
    print(flush=True)
    lines = queue.Queue()
    hung = []
    workers = [Worker(i, args, first_seed, stop_at, lines, hung) for i in range(args.workers)]
    running = len(workers)
    pending = {}
    hits = {}
    drops_by_seed = {}
    searched = [0, 0, 0]
    seeds_done = 0
    last_seed = first_seed
    last_progress = time.time()
    try:
        while running > 0:
            try:
                worker, line = lines.get(timeout=1)
            except queue.Empty:
                line = ''
                worker = None
            if worker is not None and line is None:
                if not worker.ended():
                    running -= 1
            elif line.startswith('I,'):
                row = dict(zip(ITEM_HEADER, next(csv.reader([line[2:]]))))
                if wishlist.matches(row):
                    pending.setdefault((int(row['game_seed']), int(row['difficulty'])), []).append(row)
            elif line.startswith('G,'):
                row = next(csv.reader([line[2:]]))
                seed, difficulty, skipped = int(row[0]), int(row[1]), row[2]
                searched[difficulty] += 1
                if (seed, difficulty) in pending:
                    hits[(seed, difficulty)] = pending.pop((seed, difficulty))
                    drops = drops_by_seed.setdefault(seed, {})
                    for item in hits[(seed, difficulty)]:
                        drop = drops.setdefault(same_drop(item), {'item': item, 'difficulties': set(), 'hung': skipped})
                        drop['difficulties'].add(difficulty)
                # A worker sends all three difficulties of a seed together, Hell last.
                if difficulty == 2:
                    for drop in drops_by_seed.get(seed, {}).values():
                        print_drop(drop)
                    worker.next_seed = seed + args.workers
                    seeds_done += 1
                    last_seed = max(last_seed, seed)
            elif line.startswith('HUNG,'):
                row = next(csv.reader([line[5:]]))
                hung.append((int(row[0]), int(row[1]), int(row[2])))
            elif worker is not None and line.startswith(f'worker {worker.index} finished'):
                worker.finished = True
            if time.time() - last_progress >= 60:
                print(f'... {seeds_done:,} seeds searched, up to {short_time(last_seed)}, {len(drops_by_seed):,} with a match', flush=True)
                last_progress = time.time()
    except KeyboardInterrupt:
        print('\nStopping...')
    finally:
        for worker in workers:
            if worker.process.poll() is None:
                worker.process.kill()

    print(f'\nSearched {seeds_done:,} seeds, games created {short_time(first_seed)} to {short_time(last_seed)}')
    for difficulty in range(3):
        if args.difficulty and difficulty not in args.difficulty:
            continue
        matching = sum(1 for k in hits if k[1] == difficulty)
        odds = f'1 in {searched[difficulty] / matching:,.0f}' if matching else 'none'
        print(f'  {DIFFICULTIES[difficulty]:<11}{matching:>8,} of {searched[difficulty]:,} games   {odds}')


if __name__ == '__main__':
    main()
