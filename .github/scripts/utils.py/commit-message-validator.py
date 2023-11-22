#! /bin/env python3
'''
Checks the commit messages from git-log or gh-api
'''

import argparse
import json
import logging
import re
import sys
from collections import defaultdict
import git

TITLE_LIMIT = 72
TITLE_TEXT_LIMIT = 50
TITLE_TEXT_RE = re.compile(r"([A-Z][a-z]*(-[a-z]+)*|Don't) .*[^.]$")
DESCRIPTION_LINE_LIMIT = 72
DESCRIPTION_LINE_IGNORE_RE = re.compile(r'(^Co-Authored-By:|^ *(\[[1-9][0-9]*\] |)https?://[^ ]*$)')
DIRECTORY_DEPTH = 3
DIRECTORY_SKIP = 2
OTHER_MODULE_NAMES = (
        r'CI',
        r'clang-format',
)

_OTHER_MODULE_NAMES_RE = re.compile(r'^(' + r'|'.join(OTHER_MODULE_NAMES) + r')$')

_HELP_EPILOG = '''
examples:
  Check commit messages taken from GitHub REST API:
    gh api repos/obsproject/obs-studio/pulls/5248/commits > commits.json
    %(prog)s -j commits.json
  Check commit messages from git log:
    %(prog)s -c origin/master..
'''


def find_directory_name(name, tree, max_depth):
    for t in tree.trees:
        if name == t.name:
            return True
    if max_depth > 1:
        for t in tree.trees:
            if find_directory_name(name, t, max_depth - 1):
                return True
    return False


def check_path(path, tree, allow_depth_skip):
    '''
    Check the path containing '/' exists in the tree

    The parameter allow_depth_skip sets the number of missing hierarchies to be allowed.
    For example, 'frontend/themes' is allowed though the actual path is 'frontend/data/themes'.
    '''
    paths = path.split('/', 1)
    name = paths[0]
    if len(paths) == 1:
        return find_directory_name(name, tree, allow_depth_skip + 1)

    for t in tree.trees:
        if name == t.name:
            return check_path(paths[1], t, allow_depth_skip)
        if allow_depth_skip > 0 and check_path(path, t, allow_depth_skip - 1):
            return True
    return False


def find_toplevel_file(name, blobs):
    for b in blobs:
        if name == b.name:
            return True

        name_wo_ext = b.name.rsplit('.', 1)[0]
        if name == name_wo_ext:
            return True
    return False


class SubmoduleTree:
    # pylint: disable=R0903
    '''
    This class wraps modifications for submodule so that `trees` looks like `commit.tree`.
    '''
    def __init__(self, name=None, commit=None):
        self.trees = []
        self.name = name
        if commit:
            self._initialize_with_commit_tree(commit.tree)

    def _initialize_with_commit_tree(self, tree):
        for s in tree.traverse():
            if s.type != 'submodule':
                continue
            self.add_path(s.path)

    def _get_node(self, name):
        for t in self.trees:
            if t.name == name:
                return t
        t = SubmoduleTree(name)
        self.trees.append(t)
        return t

    def add_path(self, path):
        path_split = path.split('/', 1)
        if len(path_split) == 1:
            self.trees.append(SubmoduleTree(path))
            return

        self._get_node(path_split[0]).add_path(path_split[1])

class CountLogLevelHandler(logging.Handler):
    'Count number of log messages for each log level'

    def __init__(self):
        super().__init__()
        self.counts = defaultdict(int)

    def emit(self, record):
        self.counts[record.levelno] += 1

    def has_error(self):
        if self.counts[logging.ERROR]:
            return True
        if self.counts[logging.FATAL]:
            return True
        return False

class CommitFromJson:
    # pylint: disable=R0903
    '''Helper class to provide the same interface as git.Commit.'''
    def __init__(self, obj, repo):
        self._obj = obj
        self.message = obj['commit']['message']
        self.hexsha = obj['sha']
        self.repo = repo
        # `tree` is not exactly as same as git.Commit but provided just because better than nothing.
        self.tree = repo.tree()
        self.parents = []


class Checker:
    'The implementation to check the commit messages'

    def __init__(self, repo):
        self.current_commit = None
        self.repo = repo
        self.logger = logging.getLogger('')

        self.unkown_module_name_level = logging.ERROR

        self._log_level_counter = CountLogLevelHandler()
        self.logger.addHandler(self._log_level_counter)

    @property
    def commit_abbrev(self):
        return self.current_commit.hexsha[:9]

    def check_module_name_on_commit(self, name, commit):

        if _OTHER_MODULE_NAMES_RE.match(name):
            return True
        if name.find('/') >= 0:
            if check_path(name, commit.tree, DIRECTORY_SKIP):
                return True
        else:
            if find_directory_name(name, commit.tree, DIRECTORY_DEPTH):
                return True
            if find_toplevel_file(name, commit.tree.blobs):
                return True

        submodule_tree = SubmoduleTree(commit=commit)
        if name.find('/') >= 0:
            if check_path(name, submodule_tree, DIRECTORY_SKIP):
                return True
        else:
            if find_directory_name(name, submodule_tree, DIRECTORY_DEPTH):
                return True

        return False

    def check_module_name(self, name):

        if self.check_module_name_on_commit(name, self.current_commit):
            return True

        # Also checks for removed directories.
        for commit in self.current_commit.parents:
            if self.check_module_name_on_commit(name, commit):
                return True

        self.logger.log(
                self.unkown_module_name_level,
                "commit %s: unknown module name '%s'", self.commit_abbrev, name)
        return False

    def check_module_names(self, names):
        return all(self.check_module_name(name) for name in names)

    def check_message_title(self, title):
        title_split = title.split(': ', 1)
        if len(title_split) == 2:
            self.check_module_names(re.split(r', ?', title_split[0]))
            title_text = title_split[1]
        else:
            title_text = title_split[0]

        if len(title) > TITLE_LIMIT:
            self.logger.error(
                    'commit %s: Too long title, %d characters, limit %d:\n  %s',
                    self.commit_abbrev,
                    len(title), TITLE_LIMIT, title)
        elif len(title_text) > TITLE_TEXT_LIMIT:
            self.logger.warning(
                    'commit %s: Too long title after prefix, %d characters, recommended %d:\n  %s',
                    self.commit_abbrev,
                    len(title_text), TITLE_TEXT_LIMIT, title_text)

        if not TITLE_TEXT_RE.match(title_text):
            self.logger.error(
                    'commit %s: Invalid title text:\n  %s',
                    self.commit_abbrev,
                    title_text)

    def check_message_body(self, body):
        for line in body.split('\n'):
            if DESCRIPTION_LINE_IGNORE_RE.match(line):
                continue
            if len(line) > DESCRIPTION_LINE_LIMIT and line.find(' ') > 0:
                self.logger.error(
                        'commit %s: Too long description in a line, %d characters, limit %d:\n  %s',
                        self.commit_abbrev,
                        len(line), DESCRIPTION_LINE_LIMIT, line)

    def check_message(self, c):
        self.current_commit = c
        self.logger.info(
                "Checking commit %s: '%s'",
                self.commit_abbrev,
                c.message.split('\n', 1)[0])

        msg = c.message.split('\n', 2)
        if len(msg) == 0:
            self.logger.error('commit %s: Commit message is empty.', self.commit_abbrev)
            return

        if re.match(r'Revert ', msg[0]):
            return
        if re.match(r'Merge [0-9a-f]{40} into [0-9a-f]{40}$', msg[0]):
            return
        if re.match(r'Merge pull request', msg[0]):
            return

        if len(msg) > 0:
            self.check_message_title(msg[0])
        if len(msg) > 1:
            if len(msg[1]):
                self.logger.error('commit %s: 2nd line is not empty.', self.commit_abbrev)
        if len(msg) > 2:
            self.check_message_body(msg[2])

    def check_commit(self, commit):
        self.check_message(self.repo.commit(commit))

    def check_commits(self, commits):
        for c in self.repo.iter_commits(commits):
            self.check_message(c)

    def check_by_json(self, commits):
        for obj in commits:
            try:
                c = self.repo.commit(obj['sha'])
            except:
                self.logger.info('Commit %s is not found. Using data in JSON.', obj['sha'])
                c = CommitFromJson(obj, self.repo)
            self.check_message(c)

    def has_error(self):
        'Return True if there was an error.'
        return self._log_level_counter.has_error()


def _get_args():
    parser = argparse.ArgumentParser(
            description='Log message compliance checker',
            formatter_class=argparse.RawDescriptionHelpFormatter,
            epilog=_HELP_EPILOG)
    parser.add_argument('--commit', default=None, help='One commit')
    parser.add_argument('--commits', '-c', default=None, help='Revision range')
    parser.add_argument('--json', '-j', default=None, help='JSON file to take the log messages')
    parser.add_argument('--verbose', '-v', action='count', default=0, help='Increase verbosity')
    parser.add_argument('--quiet', '-q', action='count', default=0, help='Decrease verbosity')
    parser.add_argument('--fail-never', action='store_true', default=False, help='Never fail when validation failed')
    parser.add_argument('--fail-error', action='store_true', default=True, help='Fail when error found')
    parser.add_argument('--unknown-module-error', action='store_true', default=False, help='Error if module name is unknown')
    parser.add_argument('--unknown-module-warning', action='store_true', default=False, help='Warn if module name is unknown')
    args = parser.parse_args()
    if args.fail_never:
        args.fail_error = False
    return args

def _get_log_level(args):
    verbose_level = args.verbose - args.quiet
    if verbose_level <= -2:
        return logging.FATAL
    if verbose_level <= -1:
        return logging.ERROR
    if verbose_level == 0:
        return logging.WARNING
    if verbose_level == 1:
        return logging.INFO
    return logging.DEBUG


def main():
    args = _get_args()

    logging.basicConfig(
            level = _get_log_level(args),
            format = '%(levelname)s: %(message)s',
    )

    checker = Checker(git.Repo('.'))

    if args.unknown_module_error:
        checker.unkown_module_name_level = logging.ERROR
    if args.unknown_module_warning:
        checker.unkown_module_name_level = logging.WARNING

    if args.commits:
        checker.check_commits(args.commits)

    if args.commit:
        checker.check_commit(args.commit)

    if args.json:
        if args.json == '-':
            obj = json.load(sys.stdin)
        else:
            with open(args.json, encoding='utf-8') as f:
                obj = json.load(f)

        checker.check_by_json(obj)

    if args.fail_error and checker.has_error():
        sys.exit(1)


if __name__ == '__main__':
    main()
