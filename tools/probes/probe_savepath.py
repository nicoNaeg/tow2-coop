"""Where does the exe build its save path? Looks for known-folder GUIDs and nearby strings. Read-only.
Usage: python -I probe_savepath.py <exe>"""
import sys, uuid

data = open(sys.argv[1], "rb").read()
folders = {
    "FOLDERID_SavedGames": "4C5C32FF-BB9D-43b0-B5B4-2D72E54EAAA4",
    "FOLDERID_LocalAppData": "F1B32785-6FBA-4FCF-9D55-7B8E7F157091",
    "FOLDERID_RoamingAppData": "3EB685DB-65F9-4CF6-A03A-E3EF65729F3D",
    "FOLDERID_Documents": "FDD39AD0-238F-46AF-ADB4-6C85480369C7",
    "FOLDERID_LocalAppDataLow": "A520A1A4-1780-4FF6-BD18-167343C5AF16",
}
for name, g in folders.items():
    b = uuid.UUID(g).bytes_le
    hits = []
    s = 0
    while len(hits) < 8:
        j = data.find(b, s)
        if j < 0:
            break
        hits.append(hex(j))
        s = j + 1
    print(f"{name}: {len(hits)} hit(s) {hits}")

def wide_ctx(pat, n=4):
    w = pat.encode("utf-16-le")
    s, out = 0, []
    while len(out) < n:
        j = data.find(w, s)
        if j < 0:
            break
        a = j
        while a >= 2 and 32 <= data[a - 2] < 127 and data[a - 1] == 0:
            a -= 2
        e = j
        while e + 1 < len(data) and 32 <= data[e] < 127 and data[e + 1] == 0:
            e += 2
        out.append(data[a:e].decode("utf-16-le")[:120])
        s = j + 1
    return out
for pat in ("TheOuterWorlds2", "SaveGame.dat", "Metadata.dat", "Autosave", "SaveGameScreenshot", "steam_autocloud", "UserDir", "-saveddir", "SavedGames"):
    print(f"{pat!r}: {wide_ctx(pat)}")
