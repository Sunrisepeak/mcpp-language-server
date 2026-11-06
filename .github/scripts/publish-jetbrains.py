"""Submit one verified release archive to the documented Marketplace update API."""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import re
import time
import urllib.error
import urllib.request
import uuid
import xml.etree.ElementTree as ET
import zipfile


def validate(archive, xml_id, version, sha256):
    data = archive.read_bytes()
    assert hashlib.sha256(data).hexdigest() == sha256, 'Release archive digest differs'
    assert re.fullmatch(r'[A-Za-z0-9_.-]+', xml_id)
    assert re.fullmatch(r'\d+(?:\.\d+)+(?:[-+][A-Za-z0-9.-]+)?', version)
    assert re.fullmatch(r'[A-Za-z0-9_.-]+\.(?:zip|jar)', archive.name)
    descriptors = []
    with zipfile.ZipFile(io.BytesIO(data)) as outer:
        for name in outer.namelist():
            if not name.endswith('.jar'):
                continue
            with zipfile.ZipFile(io.BytesIO(outer.read(name))) as jar:
                if 'META-INF/plugin.xml' in jar.namelist():
                    root = ET.fromstring(jar.read('META-INF/plugin.xml'))
                    descriptors.append((root.findtext('id'), root.findtext('version')))
    assert descriptors == [(xml_id, version)], descriptors
    return data


def submit(data, archive, xml_id, token, receipt):
    boundary = 'mcpp-marketplace-' + uuid.uuid4().hex
    prefix = ('--' + boundary + '\r\nContent-Disposition: form-data; name="xmlId"\r\n\r\n'
              + xml_id + '\r\n--' + boundary
              + '\r\nContent-Disposition: form-data; name="file"; filename="' + archive.name
              + '"\r\nContent-Type: application/zip\r\n\r\n').encode()
    body = prefix + data + ('\r\n--' + boundary + '--\r\n').encode()
    request = urllib.request.Request('https://plugins.jetbrains.com/api/updates/upload', body,
        headers={'Authorization': 'Bearer ' + token,
                 'Content-Type': 'multipart/form-data; boundary=' + boundary}, method='POST')
    class NoRedirect(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, request, stream, code, message, headers, new_url):
            return None
    try:
        with urllib.request.build_opener(NoRedirect).open(request, timeout=120) as response:
            raw = response.read().decode(errors='replace').replace(token, '<redacted>')
            receipt['http'] = response.status
    except urllib.error.HTTPError as error:
        raw = error.read().decode(errors='replace').replace(token, '<redacted>')
        receipt.update(http=error.code, accepted=False, error=raw[:4000])
        return False
    except (urllib.error.URLError, TimeoutError) as error:
        receipt.update(accepted=False, result_uncertain=True,
                       error='Network error during the single upload; inspect Marketplace before retrying',
                       error_type=type(error).__name__)
        return False
    try:
        result = json.loads(raw)
    except json.JSONDecodeError:
        result = {'text': raw[:4000]}
    receipt['marketplace_response'] = result
    receipt['accepted'] = receipt['http'] in (200, 201, 202) and not (isinstance(result, dict) and result.get('error'))
    return receipt['accepted']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', required=True, type=Path)
    for name in ('xml-id', 'version', 'sha256'):
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--verify-only', action='store_true')
    args = parser.parse_args()
    data = validate(args.archive, args.xml_id, args.version, args.sha256)
    receipt = dict(xml_id=args.xml_id, version=args.version, archive=args.archive.name,
                   sha256=args.sha256, bytes=len(data), verified=True,
                   utc=time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()))
    if args.verify_only:
        receipt['upload_attempted'] = False
        success = True
    else:
        token = os.environ.get('JETBRAINS_TOKEN')
        assert token, 'JETBRAINS_TOKEN is required'
        receipt['upload_attempted'] = True
        success = submit(data, args.archive, args.xml_id, token, receipt)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps(receipt))
    return 0 if success else 1


if __name__ == '__main__':
    raise SystemExit(main())
