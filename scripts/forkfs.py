"""Standalone forkfs control; requires forkfsd, never the legacy world executable."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument('--binary', default='forkfsd', help='forkfsd executable or path')
    target = result.add_mutually_exclusive_group(required=True)
    target.add_argument('--repo', type=Path, help='offline repository (must not be owned by a service)')
    target.add_argument('--socket', type=Path, help='running forkfsd RPC socket')
    view = result.add_mutually_exclusive_group()
    view.add_argument('--branch', help='writable branch name; default main')
    view.add_argument('--revision', help='immutable revision view')
    commands = result.add_subparsers(dest='command', required=True)
    for name in ['init', 'status', 'check', 'compact', 'branches', 'revisions']:
        commands.add_parser(name)
    serve = commands.add_parser('serve', help='run RPC service in foreground; Ctrl-C stops it')
    serve.add_argument('--listen', required=True, type=Path)
    for name in ['snapshot', 'restore']:
        command = commands.add_parser(name)
        command.add_argument('name')
    fork = commands.add_parser('fork')
    fork.add_argument('revision_name')
    fork.add_argument('branch_name')
    for name in ['read', 'list', 'stat', 'mkdir', 'unlink', 'rmdir']:
        command = commands.add_parser(name)
        command.add_argument('path')
    write = commands.add_parser('write')
    write.add_argument('path')
    write.add_argument('input', type=Path, help='local file containing bytes to write')
    rename = commands.add_parser('rename')
    rename.add_argument('source')
    rename.add_argument('destination')
    for name in ['mount', 'unmount']:
        command = commands.add_parser(name, help='unavailable until native mount integration is implemented')
        command.add_argument('mountpoint', type=Path)
    return result


def main(argv=None):
    command_parser = parser()
    args = command_parser.parse_args(argv)
    if args.branch == '' or args.revision == '':
        command_parser.error('branch and revision selectors must not be empty')
    if args.command in ['mount', 'unmount']:
        print('forkfs: native mount control is not implemented; no mount or directory was changed', file=sys.stderr)
        return 3
    if args.command in ['init', 'serve'] and (args.socket is not None or args.branch is not None or args.revision is not None):
        command_parser.error(f'{args.command} requires --repo without a view selector')
    if args.command in ['status', 'branches', 'revisions', 'fork'] and (args.branch is not None or args.revision is not None):
        command_parser.error(f'{args.command} does not accept a view selector')
    binary = shutil.which(args.binary)
    if not binary:
        command_parser.error(f'forkfsd executable not found: {args.binary}')
    binary = str(Path(binary).resolve())
    try:
        if args.command == 'serve':
            # Replace the script process: no detached child or second signal owner.
            os.execv(binary, [binary, 'serve', str(args.repo.resolve()), '--socket', str(args.listen.resolve()), '--rpc'])
        if args.command == 'init':
            result = subprocess.run([binary, 'init', str(args.repo.resolve())], stdout=subprocess.DEVNULL, timeout=30)
            if result.returncode:
                return exit_code(result.returncode)
            return exit_code(subprocess.run([binary, 'fs-init', str(args.repo.resolve())], timeout=30).returncode)
        operations = {'read': 'fs-cat', 'list': 'fs-ls', 'stat': 'fs-stat',
                      'mkdir': 'fs-mkdir', 'unlink': 'fs-unlink', 'rmdir': 'fs-rmdir',
                      'write': 'fs-write', 'rename': 'fs-rename', 'snapshot': 'fs-snapshot',
                      'restore': 'fs-restore', 'fork': 'fs-fork', 'branches': 'fs-worlds',
                      'revisions': 'fs-revisions', 'compact': 'fs-compact', 'check': 'fs-check'}
        operation = ('status' if args.socket else 'check') if args.command == 'status' else operations[args.command]
        arguments = []
        if hasattr(args, 'path'):
            arguments.append(args.path)
        if args.command == 'write':
            arguments.append(str(args.input.resolve()))
        if args.command == 'rename':
            arguments += [args.source, args.destination]
        if args.command in ['snapshot', 'restore']:
            arguments.append(args.name)
        if args.command == 'fork':
            arguments += [args.revision_name, args.branch_name]
        if args.branch is not None:
            arguments += ['--world', args.branch]
        elif args.revision is not None:
            arguments += ['--revision', args.revision]
        prefix = ([binary, 'request', str(args.socket.resolve()), operation] if args.socket
                  else [binary, operation, str(args.repo.resolve())])
        return exit_code(subprocess.run(prefix + arguments, timeout=30).returncode)
    except subprocess.TimeoutExpired:
        print('forkfs: command timed out; its commit outcome may need to be checked', file=sys.stderr)
        return 124
    except OSError as error:
        print(f'forkfs: {error}', file=sys.stderr)
        return 1


def exit_code(code):
    return code if code >= 0 else 128 - code


if __name__ == '__main__':
    raise SystemExit(main())
