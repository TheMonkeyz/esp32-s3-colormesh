#!/usr/bin/env python3
"""Copy a repository's GitHub settings to a new one, and check them: the settings docs/NEW-PROJECT.md section 3 lists,
through `gh api` (the GitHub CLI, signed in as the owner; nothing else talks to GitHub).

    python tools/setup_github.py --from TheMonkeyz/espforge --to TheMonkeyz/kitchen-timer --check   # compare only
    python tools/setup_github.py --from TheMonkeyz/espforge --to TheMonkeyz/kitchen-timer           # apply, then check

Settings: the repository rulesets (by name: "protect main", "release tags"), Pages built by a workflow, the github-pages
environment's deployment branch and tag policy (main, v*), and Actions' default workflow permissions (read).

--check changes nothing: it lists every setting as same or different and exits 1 when any differs. Without it the
settings --to lacks are added (each gh api call printed), a ruleset of the same name with other rules is updated, then
the comparison runs again. --from is never changed, and a setting only --to has is listed, not removed. gh: on the
PATH, else C:\\Program Files\\GitHub CLI\\gh.exe, or --gh.

The comparison is pure Python over the JSON the API returns (compare()), so tools/harness/test_setup_github.py checks it
on saved answers without a network.
"""
import argparse
import collections
import json
import os
import shutil
import subprocess
import sys

GH_WINDOWS = r'C:\Program Files\GitHub CLI\gh.exe'
# What POST/PUT /repos/{r}/rulesets take; the rest of a GET (id, node_id, source, dates, _links) names this repository
RULESET_KEYS = ('name', 'target', 'enforcement', 'bypass_actors', 'conditions', 'rules')
ENV = 'github-pages'

# One line of the report: what was compared, its status ("same", "missing", "only in ..."), and the gh api calls
# (method, path, body) that would make --to match; an empty list means nothing to apply
Row = collections.namedtuple('Row', 'what status calls')


def canon(x):
    """x with every list sorted by content, so two settings compare equal whatever order the API listed them in
    (rules, bypass actors, the ref names of a condition)."""
    if isinstance(x, dict):
        return {k: canon(v) for k, v in sorted(x.items())}
    if isinstance(x, list):
        return sorted((canon(v) for v in x), key=lambda v: json.dumps(v, sort_keys=True))
    return x


def ruleset_body(ruleset):
    """The ruleset as POST/PUT /repos/{r}/rulesets take it: without the ids, links and dates of its repository."""
    return {k: ruleset[k] for k in RULESET_KEYS if k in ruleset}


def policy_keys(policies):
    """{(name, type)} of the github-pages environment's deployment branch and tag policies."""
    return {(p.get('name'), p.get('type', 'branch')) for p in policies or []}


def compare(ref, new, repo):
    """The settings of `repo` (its snapshot `new`) against the reference snapshot `ref`: one Row per setting, in the
    order the calls must run (the environment before its policies). Pure: no network."""
    rows = []
    # Rulesets, by name
    have = {r.get('name'): r for r in new.get('rulesets') or []}
    want = {r.get('name'): r for r in ref.get('rulesets') or []}
    for name, r in want.items():
        body = ruleset_body(r)
        mine = have.get(name)
        if mine is None:
            rows.append(Row(f'ruleset "{name}"', 'missing', [('POST', f'repos/{repo}/rulesets', body)]))
        elif canon(ruleset_body(mine)) != canon(body):
            keys = [k for k in RULESET_KEYS if canon(mine.get(k)) != canon(r.get(k))]
            rows.append(Row(f'ruleset "{name}"', 'differs: ' + ', '.join(keys),
                            [('PUT', f'repos/{repo}/rulesets/{mine["id"]}', body)]))
        else:
            rows.append(Row(f'ruleset "{name}"', 'same', []))
    for name in have:
        if name not in want:
            rows.append(Row(f'ruleset "{name}"', f'only in {repo} (kept)', []))
    if not want and not have:
        rows.append(Row('rulesets', 'none in either repository', []))
    # Pages: how the site is built (a workflow deploys it)
    build = (ref.get('pages') or {}).get('build_type')
    if build:
        if new.get('pages') is None:
            rows.append(Row('pages', 'not enabled', [('POST', f'repos/{repo}/pages', {'build_type': build})]))
        elif new['pages'].get('build_type') != build:
            rows.append(Row('pages', f'build_type {new["pages"].get("build_type")}, reference {build}',
                            [('PUT', f'repos/{repo}/pages', {'build_type': build})]))
        else:
            rows.append(Row('pages', f'same (build_type {build})', []))
    elif new.get('pages') is not None:
        rows.append(Row('pages', f'only in {repo} (kept)', []))
    # The github-pages environment: which branches and tags may deploy
    env = ref.get('environment')
    if env is not None:
        policy = env.get('deployment_branch_policy')
        mine = new.get('environment')
        if mine is None:
            rows.append(Row(f'{ENV} environment', 'missing',
                            [('PUT', f'repos/{repo}/environments/{ENV}', {'deployment_branch_policy': policy})]))
        elif canon(mine.get('deployment_branch_policy')) != canon(policy):
            rows.append(Row(f'{ENV} environment',
                            f'deployment_branch_policy {json.dumps(mine.get("deployment_branch_policy"))}, '
                            f'reference {json.dumps(policy)}',
                            [('PUT', f'repos/{repo}/environments/{ENV}', {'deployment_branch_policy': policy})]))
        else:
            rows.append(Row(f'{ENV} environment', 'same', []))
        want_p, have_p = policy_keys(ref.get('policies')), policy_keys(new.get('policies'))
        for name, typ in sorted(want_p - have_p, key=str):
            rows.append(Row(f'{ENV} deployment policies', f'missing {typ} {name}',
                            [('POST', f'repos/{repo}/environments/{ENV}/deployment-branch-policies',
                              {'name': name, 'type': typ})]))
        for name, typ in sorted(have_p - want_p, key=str):
            rows.append(Row(f'{ENV} deployment policies', f'{typ} {name} only in {repo} (kept)', []))
        if want_p and want_p <= have_p:
            rows.append(Row(f'{ENV} deployment policies',
                            'same (' + ', '.join(f'{t} {n}' for n, t in sorted(want_p, key=str)) + ')', []))
    elif new.get('environment') is not None:
        rows.append(Row(f'{ENV} environment', f'only in {repo} (kept)', []))
    # Actions: what the workflow token may do by default
    perms = ref.get('workflow')
    if perms:
        if canon(new.get('workflow')) != canon(perms):
            rows.append(Row('actions workflow permissions', f'{json.dumps(new.get("workflow"))}, reference '
                            f'{json.dumps(perms)}', [('PUT', f'repos/{repo}/actions/permissions/workflow', perms)]))
        else:
            rows.append(Row('actions workflow permissions',
                            f'same ({perms.get("default_workflow_permissions")})', []))
    return rows


def differences(rows):
    """The rows that have something to apply."""
    return [r for r in rows if r.calls]


def find_gh(path=None):
    gh = path or shutil.which('gh') or (GH_WINDOWS if os.path.exists(GH_WINDOWS) else None)
    if not gh:
        raise SystemExit('gh (the GitHub CLI) not found: put it on the PATH or pass --gh <path>')
    return gh


def gh_api(gh, method, path, body=None):
    """One `gh api` call, the answer as JSON (None for 404, {} for an empty answer). Another HTTP error stops the
    tool with gh's message."""
    cmd = [gh, 'api'] + (['-X', method] if method != 'GET' else []) + [path]
    if body is not None:
        cmd += ['--input', '-']
    p = subprocess.run(cmd, input=json.dumps(body) if body is not None else None, capture_output=True, text=True,
                       encoding='utf-8')
    if p.returncode != 0:
        if 'HTTP 404' in p.stderr:
            return None
        raise SystemExit(f'gh api {method} {path} failed: {(p.stderr or p.stdout).strip()}')
    return json.loads(p.stdout) if p.stdout.strip() else {}


def snapshot(gh, repo):
    """The settings of `repo` the tool handles, read with gh api (nothing changed): what compare() takes."""
    listed = gh_api(gh, 'GET', f'repos/{repo}/rulesets?includes_parents=false') or []
    env = gh_api(gh, 'GET', f'repos/{repo}/environments/{ENV}')
    policies = gh_api(gh, 'GET', f'repos/{repo}/environments/{ENV}/deployment-branch-policies') if env else None
    return {
        'rulesets': [gh_api(gh, 'GET', f'repos/{repo}/rulesets/{r["id"]}') for r in listed],
        'pages': gh_api(gh, 'GET', f'repos/{repo}/pages'),
        'environment': env,
        'policies': (policies or {}).get('branch_policies', []),
        'workflow': gh_api(gh, 'GET', f'repos/{repo}/actions/permissions/workflow'),
    }


def report(rows, src, dst):
    print(f'{dst} against {src}:')
    for r in rows:
        print(f'  {r.what}: {r.status}')
    n = len(differences(rows))
    print(f'{n} difference{"s" if n != 1 else ""}' if n else 'no difference', flush=True)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--from', dest='src', required=True, metavar='OWNER/REPO',
                    help='the reference repository (read, never changed)')
    ap.add_argument('--to', dest='dst', required=True, metavar='OWNER/REPO', help='the repository to set up')
    ap.add_argument('--check', action='store_true', help='compare only, change nothing; exit 1 when they differ')
    ap.add_argument('--gh', help='the GitHub CLI (default: gh on the PATH, else ' + GH_WINDOWS + ')')
    opts = ap.parse_args(argv)
    if opts.src == opts.dst:
        raise SystemExit('--from and --to are the same repository')
    gh = find_gh(opts.gh)
    ref = snapshot(gh, opts.src)
    rows = compare(ref, snapshot(gh, opts.dst), opts.dst)
    report(rows, opts.src, opts.dst)
    todo = differences(rows)
    if opts.check or not todo:
        return 1 if todo else 0
    print(f'\napplying to {opts.dst}:')
    for row in todo:
        for method, path, body in row.calls:
            print(f'  {method} {path} {json.dumps(body)}', flush=True)
            gh_api(gh, method, path, body)
    print()
    rows = compare(ref, snapshot(gh, opts.dst), opts.dst)
    report(rows, opts.src, opts.dst)
    return 1 if differences(rows) else 0


if __name__ == '__main__':
    sys.exit(main())
