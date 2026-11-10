"""Verify and unpack retained full-run archives without external services."""
import argparse,hashlib,json,pathlib,tarfile
ROOT=pathlib.Path(__file__).resolve().parents[1]

def sha(path):
    with path.open('rb') as source:return hashlib.file_digest(source,'sha256').hexdigest()

def main():
    p=argparse.ArgumentParser();p.add_argument('--verify-only',action='store_true');a=p.parse_args()
    manifest=json.loads((ROOT/'evidence/archive_manifest.json').read_text())
    for item in manifest['archives']:
        path=ROOT/item['archive'];assert path.is_file(),path
        assert path.stat().st_size==item['bytes'] and sha(path)==item['sha256'],path
        print('Verified '+item['archive'],flush=True)
        if a.verify_only:continue
        destination=ROOT/item['destination'];destination.mkdir(parents=True,exist_ok=True)
        with tarfile.open(path) as archive:
            for member in archive.getmembers():
                target=(destination/member.name).resolve()
                assert target.is_relative_to(destination.resolve()),member.name
                assert member.isfile() or member.isdir(),member.name
            archive.extractall(destination,filter='data')
        print('Extracted '+item['destination'],flush=True)
    print('All retained run archives verified'+(' and extracted.' if not a.verify_only else '.'))

if __name__=='__main__':main()
