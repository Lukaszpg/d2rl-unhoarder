from pathlib import Path

root = Path(__file__).resolve().parents[1]
workflow = (root / ".github/workflows/release.yml").read_text(encoding="utf-8")

assert 'Test-Path "data" -PathType Container' in workflow
assert 'Compress-Archive -Path "data"' in workflow
assert 'dist\\unhoarder-$version-data.zip' in workflow
assert '$names=@("unhoarder.dll","unhoarder-$version-source.zip","unhoarder-$version-data.zip")' in workflow
assert 'gh release create $tag "dist\\unhoarder.dll" "dist\\unhoarder-$version-source.zip" "dist\\unhoarder-$version-data.zip" "dist\\SHA256SUMS.txt"' in workflow
assert 'path: dist/' in workflow
print("release data-folder ZIP attachment contract ok")
