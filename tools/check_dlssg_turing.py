"""Verify extern/dlssg-turing matches the dlssg-turing commit recorded by git subtree.

dlssg-turing is canonical. Change it there, then update this checkout with:
  git subtree pull --prefix extern/dlssg-turing <repository> <commit> --squash
"""
import argparse, pathlib, re, subprocess, sys
root = pathlib.Path(__file__).resolve().parents[1]
prefix = 'extern/dlssg-turing'
parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('--repo', default='https://github.com/theosw/dlssg-turing.git',
                    help='dlssg-turing repository URL or local path used to fetch the recorded commit')
args = parser.parse_args()

def git(*command, check=True):
    result = subprocess.run(['git', '-C', str(root), *command], capture_output=True, text=True)
    if check and result.returncode:
        sys.exit(f"git {' '.join(command)} failed: {result.stderr.strip()}")
    return result

if git('status', '--porcelain', '--', prefix).stdout.strip():
    sys.exit(f'{prefix} has uncommitted changes; make them in dlssg-turing and pull them instead')
message = git('log', '-1', '--format=%B', f'--grep=^git-subtree-dir: {prefix}$', 'HEAD').stdout
split = re.search(r'^git-subtree-split: ([0-9a-f]{40})$', message, re.M)
if not split:
    sys.exit(f'No git-subtree-split record found for {prefix}')
commit = split.group(1)
if git('rev-parse', '--verify', '--quiet', commit + '^{tree}', check=False).returncode:
    git('fetch', '--quiet', '--no-tags', args.repo, commit)
upstream = git('rev-parse', commit + '^{tree}').stdout.strip()
vendored = git('rev-parse', f'HEAD:{prefix}').stdout.strip()
if upstream != vendored:
    sys.exit(f'{prefix} differs from dlssg-turing {commit}; do not edit the vendored copy')
print(f'{prefix} matches dlssg-turing {commit} (tree {vendored})')
