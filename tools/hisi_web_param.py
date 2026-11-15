#!/usr/bin/env python3
"""Change fields of the camera web Parameters form.

Usage: hisi_web_param.py [--restart] NAME VALUE [NAME VALUE...]

Logs in, submits the /parameters form with every other field unchanged, and
prints each value before and after and the camera's reply. --restart uses
"Save and restart", for settings that apply only when camera-app starts.
CAM_HOST selects the camera (default 192.168.2.10); CAM_WEB_USER and
CAM_WEB_PW default to admin and ardupilot. Other tools import CameraWeb.
"""
from html.parser import HTMLParser
import http.cookiejar
import os
import re
import sys
import urllib.parse
import urllib.request


class Form(HTMLParser):
    """The fields a browser would submit from the /parameters form."""

    def __init__(self):
        super().__init__()
        self.inside = False
        self.fields = []
        self.select = None

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if tag == 'form' and a.get('action') == '/parameters':
            self.inside = True
        if not self.inside:
            return
        if tag == 'input' and a.get('name') and a.get('type') not in ('submit', 'button'):
            if a.get('type') in ('checkbox', 'radio') and 'checked' not in a:
                return
            self.fields.append([a['name'], a.get('value') or ''])
        elif tag == 'select':
            self.select = [a['name'], None]
            self.fields.append(self.select)
        elif tag == 'option' and self.select is not None:
            if self.select[1] is None or 'selected' in a:
                self.select[1] = a.get('value', '')

    def handle_endtag(self, tag):
        if tag == 'select':
            self.select = None
        if tag == 'form':
            self.inside = False


class CameraWeb:
    """A logged-in session with the camera web server."""

    def __init__(self, host=None, user=None, password=None):
        self.host = host or os.environ.get('CAM_HOST', '192.168.2.10')
        self.url = 'http://' + self.host
        self.user = user or os.environ.get('CAM_WEB_USER', 'admin')
        self.password = password or os.environ.get('CAM_WEB_PW', 'ardupilot')
        self.opener = urllib.request.build_opener(
            urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))
        self.login()

    def login(self):
        page = self.get('/login')
        token = re.search(r'name=token value="([0-9a-f]+)"', page).group(1)
        self.post('/login', {'token': token, 'username': self.user,
                             'password': self.password, 'lang': 'en'})

    def get(self, path, timeout=30):
        return self.opener.open(self.url + path, timeout=timeout).read().decode()

    def post(self, path, fields, timeout=60):
        data = urllib.parse.urlencode(fields).encode()
        return self.opener.open(self.url + path, data, timeout=timeout).read().decode()

    def form(self):
        """The Parameters form's current fields, csrf included."""
        parser = Form()
        parser.feed(self.get('/parameters'))
        fields = dict(parser.fields)
        if 'csrf' not in fields:
            raise RuntimeError(f'{self.host}: no Parameters form; is the login correct?')
        return fields

    def save(self, changes, restart=False):
        """Submit the form with CHANGES applied; return the camera's messages."""
        fields = self.form()
        unknown = sorted(set(changes) - set(fields))
        if unknown:
            raise KeyError(f'not fields of the Parameters form: {", ".join(unknown)}')
        fields.update({name: str(value) for name, value in changes.items()})
        if restart:
            fields['action'] = 'save_restart'
        return notices(self.post('/parameters', fields, timeout=120))

    def csrf(self, page):
        """The csrf token of the form posting to PAGE's first action."""
        return re.search(r'name=csrf value="([^"]+)"', self.get(page)).group(1)


def notices(page):
    return re.findall(r'class="?(?:error|notice|ok|message)[^>]*>([^<]{1,300})', page)


def main():
    args = sys.argv[1:]
    restart = args[:1] == ['--restart']
    if restart:
        args = args[1:]
    if not args or len(args) % 2:
        sys.exit(__doc__)
    changes = dict(zip(args[0::2], args[1::2]))
    web = CameraWeb()
    before = web.form()
    for name in changes:
        if name not in before:
            sys.exit(f'{name}: not a field of the Parameters form')
        print(f'{name} was {before[name]!r}')
    print('saved:', web.save(changes, restart)[:3])
    after = web.form()
    for name in changes:
        print(f'{name} now {after[name]!r}')


if __name__ == '__main__':
    main()
