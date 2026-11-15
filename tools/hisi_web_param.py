#!/usr/bin/env python3
"""Change one field of the camera web Parameters form.

Usage: hisi_web_param.py NAME VALUE

Logs in, submits the /parameters form with every other field unchanged, and
prints the value before and after. CAM_HOST selects the camera (default
192.168.2.10); CAM_WEB_USER and CAM_WEB_PW default to admin and ardupilot.
"""
from html.parser import HTMLParser
import http.cookiejar
import os
import re
import sys
import urllib.parse
import urllib.request

HOST = 'http://' + os.environ.get('CAM_HOST', '192.168.2.10')
USER = os.environ.get('CAM_WEB_USER', 'admin')
PASSWORD = os.environ.get('CAM_WEB_PW', 'ardupilot')


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


def read_form(opener):
    form = Form()
    form.feed(opener.open(HOST + '/parameters').read().decode())
    return dict(form.fields)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    name, value = sys.argv[1], sys.argv[2]
    opener = urllib.request.build_opener(
        urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))
    page = opener.open(HOST + '/login').read().decode()
    token = re.search(r'name=token value="([0-9a-f]+)"', page).group(1)
    opener.open(HOST + '/login', urllib.parse.urlencode(
        {'token': token, 'username': USER, 'password': PASSWORD, 'lang': 'en'}).encode())
    fields = read_form(opener)
    if name not in fields:
        sys.exit(f'{name}: not a field of the Parameters form '
                 f'({len(fields)} fields; is the login correct?)')
    print(f'{name} was {fields[name]!r}')
    fields[name] = value
    response = opener.open(HOST + '/parameters',
                           urllib.parse.urlencode(list(fields.items())).encode())
    messages = re.findall(r'class="?(?:error|notice|ok|message)[^>]*>([^<]{0,200})',
                          response.read().decode())
    print('saved:', response.status, messages[:3])
    print(f'{name} now {read_form(opener).get(name)!r}')


if __name__ == '__main__':
    main()
