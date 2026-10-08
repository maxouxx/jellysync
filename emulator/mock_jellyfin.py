#!/usr/bin/env python3
"""
Faux serveur Jellyfin pour tester JellySync dans l'émulateur, sans vrai serveur.

    python3 emulator/mock_jellyfin.py            # écoute sur http://127.0.0.1:8096

Dans l'app : URL = http://127.0.0.1:8096, utilisateur/mot de passe quelconques
(ou n'importe quelle clé API). Le mot de passe "bad" simule une erreur d'authentification.
Un catalogue d'environ 60 livres répartis en dossiers est servi ; chaque
téléchargement renvoie un petit fichier factice, envoyé lentement pour voir
la barre de progression. Les requêtes Range (reprise) sont acceptées.

Wi-Fi simulé : tant que /tmp/jellysync-emu-wifi-off existe (touche F9 dans
l'émulateur), le serveur ne répond plus et coupe les transferts en cours.
"""

import json
import os
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8096
USER_ID = "u-0001"
LIB_ID = "lib-books"

AUTHORS = {
    "Jules Verne": ["Vingt mille lieues sous les mers", "Le Tour du monde en quatre-vingts jours",
                    "De la Terre à la Lune", "L'Île mystérieuse", "Michel Strogoff",
                    "Voyage au centre de la Terre", "Cinq semaines en ballon"],
    "Victor Hugo": ["Les Misérables", "Notre-Dame de Paris", "Quatrevingt-treize",
                    "Les Travailleurs de la mer", "L'Homme qui rit"],
    "Alexandre Dumas": ["Les Trois Mousquetaires", "Le Comte de Monte-Cristo", "Vingt ans après",
                        "La Reine Margot", "Le Vicomte de Bragelonne"],
    "Émile Zola": ["Germinal", "L'Assommoir", "Nana", "Au Bonheur des Dames", "La Bête humaine",
                   "Le Ventre de Paris", "La Curée", "L'Œuvre"],
    "Mangas/One Piece": [f"One Piece - Tome {i:02d}" for i in range(1, 16)],
    "Science-fiction/Asimov": ["Fondation", "Fondation et Empire", "Seconde Fondation",
                               "Les Cavernes d'acier", "Les Robots"],
    "BD/Astérix": ["Astérix le Gaulois", "La Serpe d'or", "Astérix et les Goths",
                   "Astérix gladiateur", "Le Tour de Gaule d'Astérix"],
}

BOOKS = []
for folder, titles in AUTHORS.items():
    ext = ".cbz" if folder.startswith(("Mangas", "BD")) else ".epub"
    for t in titles:
        bid = f"b{len(BOOKS):04d}"
        size = 40_000 + len(BOOKS) * 1_337
        BOOKS.append({
            "Id": bid, "Name": t, "Type": "Book",
            "Path": f"/media/livres/{folder}/{t}{ext}",
            "MediaSources": [{"Size": size}], "Size": size,
        })
BY_ID = {b["Id"]: b for b in BOOKS}
WIFI_OFF_FLAG = "/tmp/jellysync-emu-wifi-off"
DELAY = float(os.environ.get("MOCK_DELAY", "0.01"))  # pause entre deux blocs de 4 Ko


def wifi_off():
    return os.path.exists(WIFI_OFF_FLAG)


class Handler(BaseHTTPRequestHandler):
    def _json(self, obj, code=200):
        data = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _drop(self):
        # Liaison coupée : on ferme sans répondre
        self.close_connection = True
        try:
            self.connection.shutdown(2)
        except OSError:
            pass

    def do_POST(self):
        if wifi_off():
            return self._drop()
        url = urlparse(self.path)
        body = self.rfile.read(int(self.headers.get("Content-Length", 0) or 0))
        if url.path == "/Users/AuthenticateByName":
            try:
                creds = json.loads(body or b"{}")
            except ValueError:
                creds = {}
            if creds.get("Pw") == "bad":
                return self._json({"error": "unauthorized"}, 401)
            return self._json({"AccessToken": "mock-token",
                               "User": {"Id": USER_ID, "Name": creds.get("Username", "max")}})
        self._json({"error": "not found"}, 404)

    def do_GET(self):
        if wifi_off():
            return self._drop()
        url = urlparse(self.path)
        q = parse_qs(url.query)
        p = url.path
        if p == "/Users/Me":
            return self._json({"Id": USER_ID, "Name": "max"})
        if p == f"/Users/{USER_ID}/Views":
            return self._json({"Items": [
                {"Id": "lib-movies", "Name": "Films", "CollectionType": "movies"},
                {"Id": LIB_ID, "Name": "Livres", "CollectionType": "books"},
            ]})
        if p == "/Items":
            start = int(q.get("startIndex", ["0"])[0])
            limit = int(q.get("limit", ["500"])[0])
            return self._json({"TotalRecordCount": len(BOOKS),
                               "Items": BOOKS[start:start + limit]})
        if p.startswith("/Items/") and p.endswith("/Download"):
            book = BY_ID.get(p.split("/")[2])
            if not book:
                return self._json({"error": "not found"}, 404)
            size = book["Size"]
            start = 0
            rng = self.headers.get("Range", "")
            if rng.startswith("bytes="):
                start = int(rng[6:].split("-")[0] or 0)
                if start >= size:
                    self.send_response(416)
                    self.send_header("Content-Range", f"bytes */{size}")
                    self.end_headers()
                    return
                self.send_response(206)
                self.send_header("Content-Range", f"bytes {start}-{size - 1}/{size}")
            else:
                self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Length", str(size - start))
            self.end_headers()
            chunk = b"x" * 4096
            sent = start
            while sent < size:
                if wifi_off():
                    return self._drop()
                n = min(len(chunk), size - sent)
                self.wfile.write(chunk[:n])
                sent += n
                time.sleep(DELAY)
            return
        self._json({"error": "not found"}, 404)

    def log_message(self, fmt, *args):
        sys.stderr.write("[mock] " + (fmt % args) + "\n")


if __name__ == "__main__":
    print(f"Faux Jellyfin sur http://127.0.0.1:{PORT} ({len(BOOKS)} livres)")
    ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
