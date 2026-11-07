# usage: tools/with-jellyfin.sh python3 tools/list_dv.py > out/titles.tsv
# TSV of the movies Jellyfin labels Dolby Vision: name, id, range type, runtime in seconds.
# Jellyfin's label is the filter, so DV files it labels otherwise (e.g. some P7 FEL) are missed.
import sys
import jf

_, uid, auth = jf.login()
items = jf.get(f"/Users/{uid}/Items?Recursive=true&IncludeItemTypes=Movie&Fields=MediaStreams", auth)["Items"]
dv = []
for it in items:
    v = [s for s in it.get("MediaStreams", []) if s.get("Type") == "Video"]
    rng = (v[0].get("VideoRangeType") or "") if v else ""
    if "DOVI" in rng.upper():
        dv.append((it["Name"], it["Id"], rng, int((it.get("RunTimeTicks") or 0) / 1e7)))
print(f"{len(dv)} Dolby Vision movies of {len(items)}", file=sys.stderr)
for d in dv:
    print("\t".join(map(str, d)))
