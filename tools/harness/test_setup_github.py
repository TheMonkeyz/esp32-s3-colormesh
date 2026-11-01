"""tools/setup_github.py without a network: its comparison of two repositories' settings on saved `gh api` answers
(the JSON esp32-s3-meteobus and espforge returned on 2026-10-31, ids shortened), and main() against a stand-in for
gh api that records every write.

    python -m unittest discover -s tools/harness -p "test_*.py"
"""
import contextlib
import copy
import io
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..'))
import setup_github as sg  # noqa: E402

REF, NEW = 'TheMonkeyz/espforge', 'TheMonkeyz/kitchen-timer'


def ruleset(name, target, include, rules, rid=1):
    """A ruleset as GET /repos/{r}/rulesets/{id} returns it."""
    return {
        'id': rid, 'name': name, 'target': target, 'source_type': 'Repository', 'source': REF,
        'enforcement': 'active', 'conditions': {'ref_name': {'exclude': [], 'include': include}}, 'rules': rules,
        'node_id': f'RRS_{rid}', 'created_at': '2026-10-09T18:58:19.157-04:00',
        'updated_at': '2026-10-09T18:58:19.214-04:00',
        'bypass_actors': [{'actor_id': 5, 'actor_type': 'RepositoryRole', 'bypass_mode': 'always'}],
        'current_user_can_bypass': 'always',
        '_links': {'self': {'href': f'https://api.github.com/repos/{REF}/rulesets/{rid}'}},
    }


PULL_REQUEST = {'type': 'pull_request', 'parameters': {
    'required_approving_review_count': 1, 'dismiss_stale_reviews_on_push': True, 'required_reviewers': [],
    'require_code_owner_review': False, 'require_last_push_approval': False,
    'required_review_thread_resolution': False, 'require_extra_approval_for_unattributed_changes': True,
    'allowed_merge_methods': ['merge', 'squash', 'rebase']}}
PROTECT_MAIN = ruleset('protect main', 'branch', ['~DEFAULT_BRANCH'],
                       [{'type': 'deletion'}, {'type': 'non_fast_forward'}, PULL_REQUEST], rid=24821128)
RELEASE_TAGS = ruleset('release tags', 'tag', ['refs/tags/v*'],
                       [{'type': 'deletion'}, {'type': 'non_fast_forward'}, {'type': 'creation'}, {'type': 'update'}],
                       rid=24821129)
PAGES = {'url': f'https://api.github.com/repos/{REF}/pages', 'status': None, 'cname': None, 'custom_404': False,
         'html_url': 'https://themonkeyz.github.io/espforge/', 'build_type': 'workflow',
         'source': {'branch': 'main', 'path': '/'}, 'public': True, 'https_enforced': True}
ENVIRONMENT = {'id': 23430724152, 'name': 'github-pages', 'can_admins_bypass': True,
               'protection_rules': [{'id': 67644240, 'type': 'branch_policy'}],
               'deployment_branch_policy': {'protected_branches': False, 'custom_branch_policies': True}}
POLICIES = [{'id': 61949871, 'name': 'main', 'type': 'branch'}, {'id': 61956620, 'name': 'v*', 'type': 'tag'}]
WORKFLOW = {'default_workflow_permissions': 'read', 'can_approve_pull_request_reviews': False}

REFERENCE = {'rulesets': [PROTECT_MAIN, RELEASE_TAGS], 'pages': PAGES, 'environment': ENVIRONMENT,
             'policies': POLICIES, 'workflow': WORKFLOW}
# A fresh repository made from the template: nothing set yet
EMPTY = {'rulesets': [], 'pages': None, 'environment': None, 'policies': [], 'workflow':
         {'default_workflow_permissions': 'write', 'can_approve_pull_request_reviews': False}}


def same_as_reference():
    """The reference's settings as another repository would return them: other ids, another order."""
    new = copy.deepcopy(REFERENCE)
    new['rulesets'].reverse()
    for i, r in enumerate(new['rulesets']):
        r['id'], r['node_id'], r['source'] = 900 + i, f'RRS_{900 + i}', NEW
        r['rules'].reverse()
        r['_links']['self']['href'] = f'https://api.github.com/repos/{NEW}/rulesets/{900 + i}'
    new['pages']['url'] = f'https://api.github.com/repos/{NEW}/pages'
    new['environment']['id'] = 42
    new['policies'] = [{'id': 7, 'name': 'v*', 'type': 'tag'}, {'id': 8, 'name': 'main', 'type': 'branch'}]
    return new


def calls(rows):
    return [c for r in rows for c in r.calls]


def status(rows, what):
    return [r.status for r in rows if r.what == what]


class Compare(unittest.TestCase):
    def test_equal_settings_have_nothing_to_apply(self):
        rows = sg.compare(REFERENCE, same_as_reference(), NEW)
        self.assertEqual(calls(rows), [])
        self.assertEqual([r.what for r in rows],
                         ['ruleset "protect main"', 'ruleset "release tags"', 'pages', 'github-pages environment',
                          'github-pages deployment policies', 'actions workflow permissions'])
        self.assertTrue(all(r.status.startswith('same') for r in rows), rows)
        self.assertEqual(status(rows, 'github-pages deployment policies'), ['same (branch main, tag v*)'])

    def test_a_missing_ruleset_is_posted_without_the_reference_ids(self):
        new = same_as_reference()
        new['rulesets'] = [r for r in new['rulesets'] if r['name'] != 'release tags']
        rows = sg.compare(REFERENCE, new, NEW)
        self.assertEqual(status(rows, 'ruleset "release tags"'), ['missing'])
        self.assertEqual(status(rows, 'ruleset "protect main"'), ['same'])
        (method, path, body), = calls(rows)
        self.assertEqual((method, path), ('POST', f'repos/{NEW}/rulesets'))
        self.assertEqual(sorted(body), sorted(sg.RULESET_KEYS))
        self.assertEqual(body['name'], 'release tags')
        self.assertEqual(body['conditions'], {'ref_name': {'exclude': [], 'include': ['refs/tags/v*']}})
        self.assertEqual(body['bypass_actors'], RELEASE_TAGS['bypass_actors'])
        for key in ('id', 'node_id', 'source', '_links', 'created_at', 'current_user_can_bypass'):
            self.assertNotIn(key, body)

    def test_a_ruleset_with_other_rules_is_updated_in_place(self):
        new = same_as_reference()
        mine = next(r for r in new['rulesets'] if r['name'] == 'protect main')
        mine['rules'] = [{'type': 'deletion'}]
        mine['enforcement'] = 'disabled'
        rows = sg.compare(REFERENCE, new, NEW)
        self.assertEqual(status(rows, 'ruleset "protect main"'), ['differs: enforcement, rules'])
        (method, path, body), = calls(rows)
        self.assertEqual((method, path), ('PUT', f'repos/{NEW}/rulesets/{mine["id"]}'))
        self.assertEqual(body, sg.ruleset_body(PROTECT_MAIN))

    def test_a_ruleset_only_the_new_repository_has_is_kept(self):
        new = same_as_reference()
        new['rulesets'].append(ruleset('no force push', 'branch', ['~ALL'], [{'type': 'non_fast_forward'}], rid=3))
        rows = sg.compare(REFERENCE, new, NEW)
        self.assertEqual(calls(rows), [])
        self.assertEqual(status(rows, 'ruleset "no force push"'), [f'only in {NEW} (kept)'])

    def test_pages_built_another_way(self):
        new = same_as_reference()
        new['pages']['build_type'] = 'legacy'
        rows = sg.compare(REFERENCE, new, NEW)
        self.assertEqual(status(rows, 'pages'), ['build_type legacy, reference workflow'])
        self.assertEqual(calls(rows), [('PUT', f'repos/{NEW}/pages', {'build_type': 'workflow'})])

    def test_pages_not_enabled(self):
        new = same_as_reference()
        new['pages'] = None
        rows = sg.compare(REFERENCE, new, NEW)
        self.assertEqual(status(rows, 'pages'), ['not enabled'])
        self.assertEqual(calls(rows), [('POST', f'repos/{NEW}/pages', {'build_type': 'workflow'})])

    def test_a_missing_tag_policy(self):
        new = same_as_reference()
        new['policies'] = [p for p in new['policies'] if p['type'] != 'tag']
        rows = sg.compare(REFERENCE, new, NEW)
        self.assertEqual(status(rows, 'github-pages environment'), ['same'])
        self.assertEqual(status(rows, 'github-pages deployment policies'), ['missing tag v*'])
        self.assertEqual(calls(rows), [('POST', f'repos/{NEW}/environments/github-pages/deployment-branch-policies',
                                        {'name': 'v*', 'type': 'tag'})])

    def test_a_policy_only_the_new_repository_has_is_kept(self):
        new = same_as_reference()
        new['policies'].append({'id': 9, 'name': 'release/*', 'type': 'branch'})
        rows = sg.compare(REFERENCE, new, NEW)
        self.assertEqual(calls(rows), [])
        self.assertEqual(status(rows, 'github-pages deployment policies'),
                         [f'branch release/* only in {NEW} (kept)', 'same (branch main, tag v*)'])

    def test_a_fresh_repository_gets_everything_in_order(self):
        rows = sg.compare(REFERENCE, EMPTY, NEW)
        got = calls(rows)
        self.assertEqual([(m, p) for m, p, _ in got], [
            ('POST', f'repos/{NEW}/rulesets'),
            ('POST', f'repos/{NEW}/rulesets'),
            ('POST', f'repos/{NEW}/pages'),
            ('PUT', f'repos/{NEW}/environments/github-pages'),
            ('POST', f'repos/{NEW}/environments/github-pages/deployment-branch-policies'),
            ('POST', f'repos/{NEW}/environments/github-pages/deployment-branch-policies'),
            ('PUT', f'repos/{NEW}/actions/permissions/workflow'),
        ])
        self.assertEqual(got[3][2], {'deployment_branch_policy': {'protected_branches': False,
                                                                 'custom_branch_policies': True}})
        self.assertEqual(sorted((b['type'], b['name']) for _, _, b in got[4:6]), [('branch', 'main'), ('tag', 'v*')])
        self.assertEqual(got[6][2], WORKFLOW)
        self.assertEqual(status(rows, 'github-pages environment'), ['missing'])
        self.assertEqual(status(rows, 'actions workflow permissions'),
                         ['{"default_workflow_permissions": "write", "can_approve_pull_request_reviews": false}, '
                          'reference {"default_workflow_permissions": "read", "can_approve_pull_request_reviews": false}'])

    def test_a_reference_without_rulesets_copies_the_rest(self):
        # espforge itself had none on 2026-10-31: the apps' rulesets are then listed as kept, and nothing is removed
        ref = dict(REFERENCE, rulesets=[])
        rows = sg.compare(ref, same_as_reference(), NEW)
        self.assertEqual(calls(rows), [])
        self.assertEqual(status(rows, 'ruleset "protect main"'), [f'only in {NEW} (kept)'])
        self.assertEqual(status(rows, 'pages'), ['same (build_type workflow)'])

    def test_canon_ignores_the_order_of_lists_only(self):
        self.assertEqual(sg.canon({'a': [{'x': 1}, {'x': 0}], 'b': [3, 1, 2]}), {'a': [{'x': 0}, {'x': 1}], 'b': [1, 2, 3]})
        self.assertNotEqual(sg.canon({'a': [1, 2]}), sg.canon({'a': [1, 2, 2]}))
        self.assertEqual(sg.canon('read'), 'read')


class FakeGh:
    """gh_api without GitHub: GET answers from the two snapshots (None where the real API answers 404), every other
    method recorded; a write to the reference repository fails the test."""

    def __init__(self, test, ref, new):
        self.test, self.writes = test, []
        self.routes = {}
        for repo, snap in ((REF, ref), (NEW, new)):
            self.routes[f'repos/{repo}/rulesets?includes_parents=false'] = [
                {k: r[k] for k in ('id', 'name', 'target', 'enforcement')} for r in snap['rulesets']]
            for r in snap['rulesets']:
                self.routes[f'repos/{repo}/rulesets/{r["id"]}'] = r
            self.routes[f'repos/{repo}/pages'] = snap['pages']
            self.routes[f'repos/{repo}/environments/github-pages'] = snap['environment']
            self.routes[f'repos/{repo}/environments/github-pages/deployment-branch-policies'] = (
                {'total_count': len(snap['policies']), 'branch_policies': snap['policies']}
                if snap['environment'] is not None else None)
            self.routes[f'repos/{repo}/actions/permissions/workflow'] = snap['workflow']

    def __call__(self, gh, method, path, body=None):
        self.test.assertEqual(gh, 'gh-test')
        if method == 'GET':
            self.test.assertIn(path, self.routes, 'an unexpected GET')
            return copy.deepcopy(self.routes[path])
        self.test.assertFalse(path.startswith(f'repos/{REF}/'), f'the reference repository was written: {method} {path}')
        self.writes.append((method, path, body))
        return {}


class Main(unittest.TestCase):
    def setUp(self):
        self.old = sg.gh_api, sg.find_gh
        sg.find_gh = lambda path=None: 'gh-test'

    def tearDown(self):
        sg.gh_api, sg.find_gh = self.old

    def run_main(self, fake, *args):
        sg.gh_api = fake
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = sg.main(['--from', REF, '--to', NEW, *args])
        return code, out.getvalue()

    def test_check_is_read_only_and_exits_1_on_a_difference(self):
        fake = FakeGh(self, REFERENCE, EMPTY)
        code, out = self.run_main(fake, '--check')
        self.assertEqual(code, 1)
        self.assertEqual(fake.writes, [])
        self.assertIn('ruleset "protect main": missing', out)
        self.assertIn('pages: not enabled', out)
        self.assertIn('7 differences', out)

    def test_check_passes_when_equal(self):
        fake = FakeGh(self, REFERENCE, same_as_reference())
        code, out = self.run_main(fake, '--check')
        self.assertEqual((code, fake.writes), (0, []))
        self.assertIn('no difference', out)

    def test_apply_writes_the_missing_settings_and_prints_each_call(self):
        fake = FakeGh(self, REFERENCE, EMPTY)
        code, out = self.run_main(fake)
        self.assertEqual([(m, p) for m, p, _ in fake.writes][:3], [
            ('POST', f'repos/{NEW}/rulesets'), ('POST', f'repos/{NEW}/rulesets'), ('POST', f'repos/{NEW}/pages')])
        self.assertEqual(len(fake.writes), 7)
        self.assertIn(f'  POST repos/{NEW}/pages {{"build_type": "workflow"}}', out)
        self.assertIn(f'applying to {NEW}:', out)
        self.assertEqual(code, 1, 'the fake does not store writes, so the check after applying still differs')

    def test_the_same_repository_twice_is_refused(self):
        with self.assertRaises(SystemExit) as cm:
            sg.main(['--from', REF, '--to', REF, '--check'])
        self.assertIn('same repository', str(cm.exception))


if __name__ == '__main__':
    unittest.main()
