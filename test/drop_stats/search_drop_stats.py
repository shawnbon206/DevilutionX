"""Simulates game seeds starting at the current second and prints the ones that drop the wanted items.

Usually run through drops.ps1 search, which prepares the simulator. Directly:
    python test/drop_stats/search_drop_stats.py --bin ~/drop-stats/search-bin --minutes 5 --type ring --prefix Gold --suffix Life

--minutes 0 searches until Ctrl+C. Ctrl+C always stops and still prints the best seeds found so far.
The workers send their items straight to this script, so a search can run for hours without using disk.

A prefix or suffix can carry a minimum for the first number it shows, e.g. --prefix Obsidian:38 Gold:28.
"""

import argparse
import csv
import datetime
import math
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
    'source_x', 'source_y', 'arrive_x', 'arrive_y',
]


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--bin', required=True, help='folder with drop_stats_test.exe and its DLLs')
    parser.add_argument('--minutes', type=float, default=5, help='how long to search; 0 searches until Ctrl+C')
    parser.add_argument('--workers', type=int, default=20, help='simulation processes; fewer leaves more CPU for playing (default 20)')
    parser.add_argument('--start', type=int, help='first seed (default: the current second)')
    parser.add_argument('--type', nargs='+', default=[], help='item types: ring amulet sword axe mace bow staff helm shield light_armor medium_armor heavy_armor')
    parser.add_argument('--base', nargs='+', default=[], help='base items, e.g. Maul "Great Axe"')
    parser.add_argument('--prefix', nargs='+', default=[], help='wanted prefixes; add :N for a minimum, e.g. Obsidian:38 means resist all at least 38')
    parser.add_argument('--suffix', nargs='+', default=[], help='wanted suffixes; add :N for a minimum, e.g. life:28')
    parser.add_argument('--unique', nargs='+', default=[], help='wanted unique items')
    parser.add_argument('--either', action='store_true', help='prefix OR suffix on the lists instead of both')
    parser.add_argument('--difficulty', nargs='+', default=[], help='normal, nightmare, hell, or 1, 2, 3 (default: all three)')
    parser.add_argument('--min-roll', type=int, help='each wanted prefix and suffix must have rolled at least this far up its range, 0-100; 80 means the top fifth')
    parser.add_argument('--full-quests', choices=['on', 'off', '1', '0'], default='on',
                        help='the "Full quests in Multiplayer" setting the host will create the game with (default on)')
    parser.add_argument('--randomize-quests', choices=['on', 'off', '1', '0'], default='on',
                        help='the Randomize Quests setting the host will create the game with (default on)')
    parser.add_argument('--seeds', type=int, default=20, help='how many of the best seeds to list at the end')
    args = parser.parse_args()
    args.full_quests = args.full_quests in ('on', '1')
    difficulties = {'normal': 0, 'nightmare': 1, 'hell': 2, '1': 0, '2': 1, '3': 2}
    for name in args.difficulty:
        if name.lower() not in difficulties:
            parser.error(f'unknown difficulty "{name}": use normal, nightmare, hell, or 1, 2, 3')
    args.difficulty = {difficulties[name.lower()] for name in args.difficulty}
    args.randomize_quests = args.randomize_quests in ('on', '1')
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


def where_in_level(item):
    """Where the source starts, as seen on screen from where you arrive by the stairs from above."""
    dx = int(item['source_x']) - int(item['arrive_x'])
    dy = int(item['source_y']) - int(item['arrive_y'])
    steps = max(abs(dx), abs(dy))
    if steps <= 2:
        return 'right where you arrive'
    # A tile step in x goes down-right on screen and a step in y down-left.
    right, down = dx - dy, dx + dy
    directions = ['right', 'up-right', 'up', 'up-left', 'left', 'down-left', 'down', 'down-right']
    direction = directions[round(math.atan2(-down, right) / (math.pi / 4)) % 8]
    return f'~{steps} steps {direction} of where you arrive'


def describe(item):
    powers = ', '.join(t for t in (item['prefix_text'], item['suffix_text']) if t)
    where = item['set_level'] or f"dlvl {item['dlvl']}"
    return f"{item['name']} ({powers}) - {where}, {item['source_kind']} {item['source_name']}, {where_in_level(item)}"


def print_game(seed, difficulty, items, hung):
    warning = f'  WARNING: the game hangs entering {hung}, stay out of it' if hung else ''
    print(f'  seed {seed} ({DIFFICULTIES[difficulty]}): {len(items)} matching{warning}')
    print(f'      created {creation_time(seed)}')
    for item in items:
        print(f'      {describe(item)}')


def main():
    args = parse_args()
    check_names(args)
    wishlist = Wishlist(args)
    first_seed = args.start if args.start is not None else int(time.time())
    stop_at = int(time.time() + args.minutes * 60) if args.minutes > 0 else None
    # Ctrl+Break stops the search the same way Ctrl+C does.
    signal.signal(signal.SIGBREAK, signal.default_int_handler)

    on_off = lambda value: 'ON' if value else 'OFF'
    print(f'For games hosted in Diablo mode with Full quests in Multiplayer {on_off(args.full_quests)} and '
          f'Randomize Quests {on_off(args.randomize_quests)}; the host must match.')
    how_long = f'for {args.minutes:g} minutes' if stop_at else 'until Ctrl+C'
    print(f'Searching from {creation_time(first_seed)} {how_long} on {args.workers} workers; Ctrl+C stops and prints the best seeds.\n', flush=True)
    lines = queue.Queue()
    hung = []
    workers = [Worker(i, args, first_seed, stop_at, lines, hung) for i in range(args.workers)]
    running = len(workers)
    pending = {}
    hits = {}
    hung_levels = {}
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
                    hung_levels[(seed, difficulty)] = skipped
                    print_game(seed, difficulty, hits[(seed, difficulty)], skipped)
                if difficulty == 2:
                    worker.next_seed = seed + args.workers
                    seeds_done += 1
                    last_seed = max(last_seed, seed)
            elif line.startswith('HUNG,'):
                row = next(csv.reader([line[5:]]))
                hung.append((int(row[0]), int(row[1]), int(row[2])))
            elif worker is not None and line.startswith(f'worker {worker.index} finished'):
                worker.finished = True
            if time.time() - last_progress >= 60:
                print(f'... {seeds_done:,} seeds searched, up to games created {creation_time(last_seed)}; {len({k[0] for k in hits}):,} with a match', flush=True)
                last_progress = time.time()
    except KeyboardInterrupt:
        print('\nStopping...')
    finally:
        for worker in workers:
            if worker.process.poll() is None:
                worker.process.kill()

    print(f'\nSearched {seeds_done:,} seeds, games created {creation_time(first_seed)} to {creation_time(last_seed)}.')
    for difficulty in range(3):
        if args.difficulty and difficulty not in args.difficulty:
            continue
        matching = sum(1 for k in hits if k[1] == difficulty)
        odds = f'1 in {searched[difficulty] / matching:,.0f}' if matching else 'none'
        print(f'  {DIFFICULTIES[difficulty]:<10} {matching:,} of {searched[difficulty]:,} games have a match ({odds})')
    if hits:
        print(f'\nBest seeds (up to {args.seeds}):')
        best = sorted(hits, key=lambda k: (-len(hits[k]), k[0]))[:args.seeds]
        for key in best:
            print_game(key[0], key[1], hits[key], hung_levels.get(key, ''))


if __name__ == '__main__':
    main()
