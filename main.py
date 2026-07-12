"""English flash-card service for a 4.2-inch tri-colour EPD."""

from __future__ import annotations

import base64
import asyncio
import io
import os
import re
import sqlite3
import textwrap
import unicodedata
from contextlib import asynccontextmanager
from datetime import UTC, datetime
from pathlib import Path

import httpx
from fastapi import BackgroundTasks, FastAPI, HTTPException
from fastapi.responses import FileResponse, HTMLResponse
from fastapi.staticfiles import StaticFiles
from openai import OpenAI
from PIL import Image, ImageDraw, ImageFont, ImageOps
from pydantic import BaseModel, Field

ROOT = Path(__file__).parent
DATA_DIR, IMAGE_DIR = ROOT / "data", ROOT / "data" / "cards"
DB_PATH = DATA_DIR / "flashcards.db"
WIDTH, HEIGHT = 400, 300
BLACK, WHITE, RED = (0, 0, 0), (255, 255, 255), (210, 30, 45)
EPD_PANEL_TYPE, EPD_COLOR_MODE = "gdey042z98", "tricolor"
EPD_FRAME_BYTES = WIDTH * HEIGHT // 4
VOWELS = set("aeiouAEIOU")
BATCH_RUNNING = False
MAX_CARD_HINT_LENGTH = 52
EPD_SEND_LOCK = asyncio.Lock()


class CardInput(BaseModel):
    word: str = Field(min_length=1, max_length=60)
    ipa: str = ""
    syllables: str = ""
    hint: str = ""


class CardCreate(CardInput):
    book_ids: list[int] = []


class BulkCardCreate(BaseModel):
    words: str = Field(min_length=1, max_length=20_000)
    book_ids: list[int] = []


class BookInput(BaseModel):
    name: str = Field(min_length=1, max_length=80)


class MembershipInput(BaseModel):
    book_ids: list[int]


class EnrichInput(BaseModel):
    word: str = Field(min_length=1, max_length=60)


class EpdAutoRefreshInput(BaseModel):
    enabled: bool = False
    book_id: int | None = None
    interval_minutes: int = Field(default=10, ge=1, le=1440)


def now() -> str:
    return datetime.now(UTC).isoformat()


def connection() -> sqlite3.Connection:
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA foreign_keys = ON")
    return conn


def add_column(conn: sqlite3.Connection, name: str, definition: str) -> None:
    columns = {row[1] for row in conn.execute("PRAGMA table_info(cards)")}
    if name not in columns:
        conn.execute(f"ALTER TABLE cards ADD COLUMN {name} {definition}")


def initialize() -> None:
    DATA_DIR.mkdir(exist_ok=True)
    IMAGE_DIR.mkdir(exist_ok=True)
    with connection() as conn:
        conn.execute("""CREATE TABLE IF NOT EXISTS cards (
            id INTEGER PRIMARY KEY AUTOINCREMENT, word TEXT NOT NULL UNIQUE,
            ipa TEXT NOT NULL DEFAULT '', syllables TEXT NOT NULL DEFAULT '', hint TEXT NOT NULL DEFAULT '',
            image_path TEXT, review_count INTEGER NOT NULL DEFAULT 0, last_reviewed_at TEXT, created_at TEXT NOT NULL
        )""")
        add_column(conn, "image_status", "TEXT NOT NULL DEFAULT 'pending'")
        add_column(conn, "image_error", "TEXT")
        conn.execute("""CREATE TABLE IF NOT EXISTS books (
            id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL UNIQUE, created_at TEXT NOT NULL
        )""")
        conn.execute("""CREATE TABLE IF NOT EXISTS card_books (
            card_id INTEGER NOT NULL REFERENCES cards(id) ON DELETE CASCADE,
            book_id INTEGER NOT NULL REFERENCES books(id) ON DELETE CASCADE,
            PRIMARY KEY (card_id, book_id)
        )""")
        conn.execute("""CREATE TABLE IF NOT EXISTS epd_auto_refresh (
            id INTEGER PRIMARY KEY CHECK (id=1),
            enabled INTEGER NOT NULL DEFAULT 0,
            book_id INTEGER REFERENCES books(id) ON DELETE SET NULL,
            interval_minutes INTEGER NOT NULL DEFAULT 10,
            last_card_id INTEGER REFERENCES cards(id) ON DELETE SET NULL,
            last_attempt_at TEXT,
            last_sent_at TEXT,
            last_error TEXT
        )""")
        conn.execute("INSERT OR IGNORE INTO epd_auto_refresh (id) VALUES (1)")


@asynccontextmanager
async def lifespan(_: FastAPI):
    initialize()
    task = asyncio.create_task(auto_refresh_loop())
    try:
        yield
    finally:
        task.cancel()
        try:
            await task
        except asyncio.CancelledError:
            pass


app = FastAPI(title="EPD English Flash Cards", lifespan=lifespan)
app.mount("/static", StaticFiles(directory=ROOT / "static"), name="static")


def get_card(card_id: int) -> sqlite3.Row:
    with connection() as conn:
        row = conn.execute("SELECT * FROM cards WHERE id=?", (card_id,)).fetchone()
    if not row:
        raise HTTPException(404, "Flash card not found")
    return row


def card_books(conn: sqlite3.Connection, card_id: int) -> list[dict]:
    rows = conn.execute(
        """SELECT b.id, b.name FROM books b JOIN card_books cb ON cb.book_id=b.id
                         WHERE cb.card_id=? ORDER BY b.name""",
        (card_id,),
    ).fetchall()
    return [dict(row) for row in rows]


def serialize(row: sqlite3.Row, conn: sqlite3.Connection | None = None) -> dict:
    card = dict(row)
    path = Path(card["image_path"]) if card["image_path"] else None
    card["image_url"] = (
        f"/api/cards/{card['id']}/image" if path and path.exists() else None
    )
    card.pop("image_path", None)
    if conn:
        card["books"] = card_books(conn, card["id"])
    return card


def ensure_book_ids(conn: sqlite3.Connection, book_ids: list[int]) -> list[int]:
    ids = sorted(set(book_ids))
    if ids and conn.execute(
        f"SELECT count(*) FROM books WHERE id IN ({','.join('?' * len(ids))})", ids
    ).fetchone()[0] != len(ids):
        raise HTTPException(422, "One or more word books do not exist")
    return ids


def set_memberships(
    conn: sqlite3.Connection, card_id: int, book_ids: list[int]
) -> None:
    ids = ensure_book_ids(conn, book_ids)
    conn.execute("DELETE FROM card_books WHERE card_id=?", (card_id,))
    conn.executemany(
        "INSERT INTO card_books (card_id, book_id) VALUES (?, ?)",
        [(card_id, book_id) for book_id in ids],
    )


def auto_refresh_settings() -> dict:
    with connection() as conn:
        row = conn.execute("SELECT * FROM epd_auto_refresh WHERE id=1").fetchone()
        eligible_cards = conn.execute(
            """SELECT count(DISTINCT c.id) FROM cards c
               LEFT JOIN card_books cb ON cb.card_id=c.id
               WHERE c.image_status='ready' AND c.image_path IS NOT NULL
                 AND (? IS NULL OR cb.book_id=?)""",
            (row["book_id"], row["book_id"]),
        ).fetchone()[0]
        book = (
            conn.execute("SELECT name FROM books WHERE id=?", (row["book_id"],)).fetchone()
            if row["book_id"] is not None
            else None
        )
        card = (
            conn.execute("SELECT word FROM cards WHERE id=?", (row["last_card_id"],)).fetchone()
            if row["last_card_id"] is not None
            else None
        )
    settings = dict(row)
    settings["enabled"] = bool(settings["enabled"])
    settings["eligible_cards"] = eligible_cards
    settings["book_name"] = book["name"] if book else None
    settings["last_card_word"] = card["word"] if card else None
    return settings


def auto_refresh_due(settings: dict) -> bool:
    if not settings["enabled"] or not settings["last_attempt_at"]:
        return settings["enabled"]
    last_attempt = datetime.fromisoformat(settings["last_attempt_at"])
    return (datetime.now(UTC) - last_attempt).total_seconds() >= settings["interval_minutes"] * 60


def random_ready_card_id(book_id: int | None) -> int | None:
    with connection() as conn:
        rows = conn.execute(
            """SELECT DISTINCT c.id, c.image_path FROM cards c
               LEFT JOIN card_books cb ON cb.card_id=c.id
               WHERE c.image_status='ready' AND c.image_path IS NOT NULL
                 AND (? IS NULL OR cb.book_id=?) ORDER BY RANDOM()""",
            (book_id, book_id),
        ).fetchall()
    return next((row["id"] for row in rows if Path(row["image_path"]).exists()), None)


async def run_auto_refresh() -> None:
    settings = auto_refresh_settings()
    if not settings["enabled"]:
        return
    card_id = random_ready_card_id(settings["book_id"])
    attempted_at = now()
    if card_id is None:
        with connection() as conn:
            conn.execute(
                "UPDATE epd_auto_refresh SET last_attempt_at=?, last_error=? WHERE id=1",
                (attempted_at, "没有已生成、可发送的闪卡"),
            )
        return
    try:
        await send_card_to_epd(card_id)
    except HTTPException as exc:
        with connection() as conn:
            conn.execute(
                "UPDATE epd_auto_refresh SET last_attempt_at=?, last_error=? WHERE id=1",
                (attempted_at, str(exc.detail)[:300]),
            )
        return
    with connection() as conn:
        conn.execute(
            """UPDATE epd_auto_refresh
               SET last_attempt_at=?, last_sent_at=?, last_card_id=?, last_error=NULL WHERE id=1""",
            (attempted_at, attempted_at, card_id),
        )


async def auto_refresh_loop() -> None:
    """Keep the EPD rotation alive independently from an open browser tab."""
    while True:
        await asyncio.sleep(5)
        try:
            settings = auto_refresh_settings()
            if auto_refresh_due(settings):
                await run_auto_refresh()
        except Exception:
            # Individual failures are stored by run_auto_refresh; keep the scheduler alive.
            pass


@app.get("/", response_class=HTMLResponse)
def index() -> HTMLResponse:
    return HTMLResponse((ROOT / "templates" / "index.html").read_text())


@app.get("/api/health")
def health() -> dict:
    return {
        "ok": True,
        "openai_configured": bool(os.getenv("OPENAI_API_KEY")),
        "epd_configured": bool(os.getenv("EPD_UPLOAD_URL")),
    }


@app.get("/api/epd/auto-refresh")
def get_auto_refresh() -> dict:
    return auto_refresh_settings()


@app.put("/api/epd/auto-refresh")
def update_auto_refresh(payload: EpdAutoRefreshInput) -> dict:
    with connection() as conn:
        if payload.book_id is not None and not conn.execute(
            "SELECT 1 FROM books WHERE id=?", (payload.book_id,)
        ).fetchone():
            raise HTTPException(422, "Word book does not exist")
        conn.execute(
            """UPDATE epd_auto_refresh
               SET enabled=?, book_id=?, interval_minutes=?, last_attempt_at=NULL,
                   last_error=NULL WHERE id=1""",
            (payload.enabled, payload.book_id, payload.interval_minutes),
        )
    return auto_refresh_settings()


@app.get("/api/books")
def list_books() -> list[dict]:
    with connection() as conn:
        rows = conn.execute(
            """SELECT b.id, b.name, b.created_at, count(cb.card_id) AS card_count
                             FROM books b LEFT JOIN card_books cb ON cb.book_id=b.id
                             GROUP BY b.id ORDER BY b.name"""
        ).fetchall()
    return [dict(row) for row in rows]


@app.post("/api/books", status_code=201)
def create_book(payload: BookInput) -> dict:
    try:
        with connection() as conn:
            cursor = conn.execute(
                "INSERT INTO books (name, created_at) VALUES (?, ?)",
                (payload.name.strip(), now()),
            )
            return dict(
                conn.execute(
                    "SELECT id, name, created_at, 0 AS card_count FROM books WHERE id=?",
                    (cursor.lastrowid,),
                ).fetchone()
            )
    except sqlite3.IntegrityError as exc:
        raise HTTPException(409, "This word book already exists") from exc


@app.delete("/api/books/{book_id}", status_code=204)
def delete_book(book_id: int) -> None:
    with connection() as conn:
        if not conn.execute("SELECT 1 FROM books WHERE id=?", (book_id,)).fetchone():
            raise HTTPException(404, "Word book not found")
        conn.execute("DELETE FROM books WHERE id=?", (book_id,))


@app.get("/api/cards")
def list_cards(
    book_id: int | None = None,
    q: str = "",
    sort: str = "created_desc",
    page: int = 1,
    page_size: int = 24,
) -> dict:
    sort_order = {
        "created_desc": "c.id DESC",
        "created_asc": "c.id ASC",
        "word_asc": "lower(c.word) ASC",
        "word_desc": "lower(c.word) DESC",
        "review_desc": "c.review_count DESC, c.id DESC",
    }.get(sort, "c.id DESC")
    page, page_size = max(page, 1), min(max(page_size, 1), 100)
    with connection() as conn:
        joins, clauses, params = [], [], []
        if book_id is not None:
            joins.append("JOIN card_books cb ON cb.card_id=c.id")
            clauses.append("cb.book_id=?")
            params.append(book_id)
        if q.strip():
            clauses.append("lower(c.word) LIKE ?")
            params.append(f"%{q.strip().casefold()}%")
        source = "FROM cards c " + " ".join(joins)
        where = (" WHERE " + " AND ".join(clauses)) if clauses else ""
        total = conn.execute("SELECT count(DISTINCT c.id) " + source + where, params).fetchone()[0]
        offset = (page - 1) * page_size
        rows = conn.execute("SELECT DISTINCT c.* " + source + where + f" ORDER BY {sort_order} LIMIT ? OFFSET ?", [*params, page_size, offset]).fetchall()
        return {"items": [serialize(row, conn) for row in rows], "total": total, "page": page, "page_size": page_size, "total_pages": max((total + page_size - 1) // page_size, 1)}


@app.post("/api/cards", status_code=201)
def create_card(payload: CardCreate) -> dict:
    word = payload.word.strip()
    try:
        with connection() as conn:
            if conn.execute(
                "SELECT 1 FROM cards WHERE lower(word)=lower(?)", (word,)
            ).fetchone():
                raise HTTPException(409, "This word already exists")
            cursor = conn.execute(
                """INSERT INTO cards (word, ipa, syllables, hint, created_at, image_status)
                                   VALUES (?, ?, ?, ?, ?, 'pending')""",
                (
                    word,
                    payload.ipa.strip(),
                    payload.syllables.strip(),
                    payload.hint.strip(),
                    now(),
                ),
            )
            set_memberships(conn, cursor.lastrowid, payload.book_ids)
            return serialize(
                conn.execute(
                    "SELECT * FROM cards WHERE id=?", (cursor.lastrowid,)
                ).fetchone(),
                conn,
            )
    except sqlite3.IntegrityError as exc:
        raise HTTPException(409, "This word already exists") from exc


def normalize_bulk_word(raw: str) -> str:
    """Normalize pasted lists while retaining English phrase wording and casing."""
    word = unicodedata.normalize("NFKC", raw).strip()
    word = re.sub(r"^(?:[-*•]+|\d+[.)])\s*", "", word)
    word = word.replace("’", "'").replace("‘", "'").replace("–", "-").replace("—", "-")
    return re.sub(r"\s+", " ", word).strip(" \t\"'.,，。!！?？:：;；")


@app.post("/api/cards/bulk", status_code=201)
def create_cards_bulk(payload: BulkCardCreate, background_tasks: BackgroundTasks) -> dict:
    """Create cards from one pasted item per line, preserving valid phrases/capitalization."""
    candidates: list[str] = []
    skipped: list[dict] = []
    seen: set[str] = set()
    for line_number, raw in enumerate(payload.words.splitlines(), start=1):
        word = normalize_bulk_word(raw)
        key = word.casefold()
        if not word:
            continue
        if not re.search(r"[A-Za-z]", word) or len(word) > 60:
            skipped.append({"line": line_number, "word": raw, "reason": "not a valid English word or phrase"})
        elif key in seen:
            skipped.append({"line": line_number, "word": word, "reason": "duplicate in pasted list"})
        else:
            seen.add(key)
            candidates.append(word)

    created: list[dict] = []
    with connection() as conn:
        book_ids = ensure_book_ids(conn, payload.book_ids)
        for word in candidates:
            if conn.execute("SELECT 1 FROM cards WHERE lower(word)=lower(?)", (word,)).fetchone():
                skipped.append({"word": word, "reason": "already exists"})
                continue
            cursor = conn.execute(
                "INSERT INTO cards (word, created_at, image_status) VALUES (?, ?, 'pending')",
                (word, now()),
            )
            set_memberships(conn, cursor.lastrowid, book_ids)
            created.append(serialize(conn.execute("SELECT * FROM cards WHERE id=?", (cursor.lastrowid,)).fetchone(), conn))
    created_ids = [card["id"] for card in created]
    if created_ids:
        background_tasks.add_task(enrich_cards, created_ids)
    return {"created": created, "created_count": len(created), "skipped": skipped, "enrichment_queued": len(created_ids)}


@app.put("/api/cards/{card_id}")
def update_card(card_id: int, payload: CardInput) -> dict:
    card = get_card(card_id)
    with connection() as conn:
        changed_word = card["word"].casefold() != payload.word.strip().casefold()
        duplicate = conn.execute(
            "SELECT 1 FROM cards WHERE id<>? AND lower(word)=lower(?)",
            (card_id, payload.word.strip()),
        ).fetchone()
        if duplicate:
            raise HTTPException(409, "This word already exists")
        conn.execute(
            """UPDATE cards SET word=?, ipa=?, syllables=?, hint=?, image_path=?, image_status=?, image_error=? WHERE id=?""",
            (
                payload.word.strip(),
                payload.ipa.strip(),
                payload.syllables.strip(),
                payload.hint.strip(),
                None if changed_word else card["image_path"],
                "pending" if changed_word else card["image_status"],
                None,
                card_id,
            ),
        )
        return serialize(
            conn.execute("SELECT * FROM cards WHERE id=?", (card_id,)).fetchone(), conn
        )


@app.put("/api/cards/{card_id}/books")
def update_card_books(card_id: int, payload: MembershipInput) -> dict:
    get_card(card_id)
    with connection() as conn:
        set_memberships(conn, card_id, payload.book_ids)
        return serialize(
            conn.execute("SELECT * FROM cards WHERE id=?", (card_id,)).fetchone(), conn
        )


@app.delete("/api/cards/{card_id}", status_code=204)
def delete_card(card_id: int) -> None:
    card = get_card(card_id)
    if card["image_path"]:
        Path(card["image_path"]).unlink(missing_ok=True)
    with connection() as conn:
        conn.execute("DELETE FROM cards WHERE id=?", (card_id,))


def syllabify(word: str) -> str:
    """A lightweight fallback for English word syllables when a dictionary has no splits."""
    parts = re.findall(
        r"[^aeiouy]*[aeiouy]+(?:[^aeiouy](?=[aeiouy])|[^aeiouy]$)?", word.lower()
    )
    return "-".join(parts) if len(parts) > 1 else word


def enrichment_for(word: str) -> dict:
    result = {
        "word": word,
        "ipa": "",
        "syllables": syllabify(word),
        "hint": "",
        "source": "syllable fallback",
    }
    try:
        response = httpx.get(
            f"https://api.dictionaryapi.dev/api/v2/entries/en/{word}", timeout=5
        )
        response.raise_for_status()
        entry = response.json()[0]
        phonetics = entry.get("phonetics", [])
        result["ipa"] = next(
            (item.get("text", "") for item in phonetics if item.get("text")),
            entry.get("phonetic", ""),
        )
        meanings = entry.get("meanings", [])
        if meanings and meanings[0].get("definitions"):
            result["hint"] = meanings[0]["definitions"][0].get("definition", "")
        result["source"] = "dictionaryapi.dev + syllable fallback"
    except (httpx.HTTPError, ValueError, IndexError, KeyError):
        result["hint"] = f"a simple picture of {word}"
    return result


def enrich_cards(card_ids: list[int]) -> None:
    """Fill only blank fields so background enrichment never overwrites edits."""
    for card_id in card_ids:
        try:
            card = get_card(card_id)
            suggestion = enrichment_for(card["word"])
            with connection() as conn:
                conn.execute(
                    """UPDATE cards SET ipa=CASE WHEN ipa='' THEN ? ELSE ipa END,
                       syllables=CASE WHEN syllables='' THEN ? ELSE syllables END,
                       hint=CASE WHEN hint='' THEN ? ELSE hint END WHERE id=?""",
                    (suggestion["ipa"], suggestion["syllables"], suggestion["hint"], card_id),
                )
        except (HTTPException, sqlite3.Error):
            continue


@app.post("/api/cards/enrich")
def enrich_word(payload: EnrichInput) -> dict:
    return enrichment_for(payload.word.strip())


def load_font(size: int) -> ImageFont.FreeTypeFont:
    for path in (
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    ):
        if Path(path).exists():
            return ImageFont.truetype(path, size)
    return ImageFont.load_default()


def draw_coloured_word(draw: ImageDraw.ImageDraw, word: str, y: int) -> None:
    font = load_font(54)
    widths = [int(draw.textlength(char, font=font)) for char in word]
    x = (WIDTH - sum(widths)) // 2
    for char, width in zip(word, widths):
        draw.text((x, y), char, font=font, fill=RED if char in VOWELS else BLACK)
        x += width


def child_friendly_hint(hint: str) -> str:
    """Keep a short memory cue, but never render dictionary-definition prose."""
    clean = " ".join(hint.split())
    if len(clean) > MAX_CARD_HINT_LENGTH or any(mark in clean for mark in (";", ":", "(", ")")):
        return ""
    return clean


def ai_illustration(card: sqlite3.Row) -> Image.Image:
    if not (api_key := os.getenv("OPENAI_API_KEY")):
        raise RuntimeError("OPENAI_API_KEY is not configured")
    hint = child_friendly_hint(card["hint"])
    memory_cue = f" Memory cue: {hint}." if hint else ""
    prompt = (
        f"A simple child-friendly memory illustration of {card['word']}.{memory_cue} "
        "Centered single object, large clear silhouette, black ink line art with small red accents only, "
        "pure white background, no letters, no words, no numbers, no border, screen-print style."
    )
    response = OpenAI(api_key=api_key).images.generate(
        model=os.getenv("OPENAI_IMAGE_MODEL", "gpt-image-2"),
        prompt=prompt,
        size="1024x1024",
        quality="low",
    )
    encoded = response.data[0].b64_json
    if not encoded:
        raise RuntimeError("OpenAI did not return image data")
    return Image.open(io.BytesIO(base64.b64decode(encoded))).convert("RGB")


def image_filename(word: str) -> str:
    name = re.sub(r"[^a-z0-9]+", "-", word.casefold()).strip("-")
    return f"{name or 'flashcard'}.png"


def nearest_epd_colour(pixel: tuple[int, int, int]) -> tuple[int, tuple[int, int, int]]:
    """Return the ESP32 2-bit colour code for the 4.2-inch tri-colour panel."""
    choices = ((0, WHITE), (2, RED), (3, BLACK))
    return min(
        choices,
        key=lambda choice: sum(
            (component - target) ** 2 for component, target in zip(pixel, choice[1])
        ),
    )


def epd_frame(image_path: Path) -> bytes:
    """Pack a PNG into the four-pixels-per-byte format expected by /api/frame."""
    with Image.open(image_path) as source:
        image = source.convert("RGB")
    if image.size != (WIDTH, HEIGHT):
        image = ImageOps.contain(image, (WIDTH, HEIGHT))
        canvas = Image.new("RGB", (WIDTH, HEIGHT), WHITE)
        canvas.paste(image, ((WIDTH - image.width) // 2, (HEIGHT - image.height) // 2))
        image = canvas

    frame = bytearray(EPD_FRAME_BYTES)
    for index, pixel in enumerate(image.getdata()):
        code, _ = nearest_epd_colour(pixel)
        frame[index // 4] |= code << (6 - (index % 4) * 2)
    return bytes(frame)


def compose_card(card: sqlite3.Row, illustration: Image.Image) -> Path:
    canvas = Image.new("RGB", (WIDTH, HEIGHT), WHITE)
    draw = ImageDraw.Draw(canvas)
    draw_coloured_word(draw, card["word"], 14)
    meta_font, hint_font = load_font(17), load_font(14)
    draw.text(
        (WIDTH // 2, 78),
        f"{card['syllables'] or 'Add syllables'}  •  {card['ipa'] or 'Add IPA'}",
        anchor="ma",
        font=meta_font,
        fill=BLACK,
    )
    draw.line((28, 104, 372, 104), fill=BLACK, width=1)
    art = ImageOps.contain(illustration, (220, 145))
    canvas.paste(art, ((WIDTH - art.width) // 2, 116))
    if hint := child_friendly_hint(card["hint"]):
        draw.rounded_rectangle(
            (15, 269, 385, 292), radius=6, fill=WHITE, outline=BLACK, width=1
        )
        draw.text(
            (WIDTH // 2, 281),
            hint,
            anchor="mm",
            font=hint_font,
            fill=BLACK,
        )
    path = IMAGE_DIR / image_filename(card["word"])
    palette = Image.new("P", (1, 1))
    palette.putpalette(BLACK + WHITE + RED + (0, 0, 0) * 253)
    canvas.quantize(palette=palette, dither=Image.Dither.NONE).save(path)
    return path


def generate_one(card_id: int, force: bool) -> dict:
    card = get_card(card_id)
    expected_path = IMAGE_DIR / image_filename(card["word"])
    if not force and expected_path.exists():
        with connection() as conn:
            conn.execute(
                "UPDATE cards SET image_path=?, image_status='ready', image_error=NULL WHERE id=?",
                (str(expected_path), card_id),
            )
            return serialize(
                conn.execute("SELECT * FROM cards WHERE id=?", (card_id,)).fetchone(),
                conn,
            )
    with connection() as conn:
        conn.execute(
            "UPDATE cards SET image_status='generating', image_error=NULL WHERE id=?",
            (card_id,),
        )
    try:
        path = compose_card(card, ai_illustration(card))
        with connection() as conn:
            conn.execute(
                "UPDATE cards SET image_path=?, image_status='ready', image_error=NULL WHERE id=?",
                (str(path), card_id),
            )
            return serialize(
                conn.execute("SELECT * FROM cards WHERE id=?", (card_id,)).fetchone(),
                conn,
            )
    except Exception as exc:
        with connection() as conn:
            conn.execute(
                "UPDATE cards SET image_status='failed', image_error=? WHERE id=?",
                (str(exc)[:300], card_id),
            )
        raise HTTPException(502, f"Image generation failed: {exc}") from exc


@app.post("/api/cards/{card_id}/generate")
def generate_card(card_id: int) -> dict:
    """This endpoint intentionally forces an image regeneration."""
    return generate_one(card_id, force=True)


def run_batch(card_ids: list[int]) -> None:
    global BATCH_RUNNING
    try:
        for card_id in card_ids:
            try:
                generate_one(card_id, force=False)
            except HTTPException:
                pass
    finally:
        BATCH_RUNNING = False


@app.post("/api/images/generate-batch", status_code=202)
def generate_batch(
    background_tasks: BackgroundTasks, book_id: int | None = None
) -> dict:
    global BATCH_RUNNING
    if BATCH_RUNNING:
        raise HTTPException(409, "A batch generation job is already running")
    with connection() as conn:
        query, params = "SELECT DISTINCT c.id FROM cards c", []
        if book_id is not None:
            query += " JOIN card_books cb ON cb.card_id=c.id WHERE cb.book_id=?"
            params = [book_id]
        card_ids = [row["id"] for row in conn.execute(query, params).fetchall()]
    if not card_ids:
        raise HTTPException(404, "No cards to generate")
    BATCH_RUNNING = True
    background_tasks.add_task(run_batch, card_ids)
    return {"ok": True, "queued": len(card_ids)}


@app.get("/api/images/progress")
def image_progress(book_id: int | None = None) -> dict:
    with connection() as conn:
        query, params = "SELECT c.image_status, count(*) AS total FROM cards c", []
        if book_id is not None:
            query += " JOIN card_books cb ON cb.card_id=c.id WHERE cb.book_id=?"
            params = [book_id]
        rows = conn.execute(query + " GROUP BY c.image_status", params).fetchall()
    counts = {row["image_status"]: row["total"] for row in rows}
    total = sum(counts.values())
    return {
        "running": BATCH_RUNNING,
        "total": total,
        "ready": counts.get("ready", 0),
        "generating": counts.get("generating", 0),
        "pending": counts.get("pending", 0),
        "failed": counts.get("failed", 0),
    }


@app.get("/api/cards/{card_id}/image")
def card_image(card_id: int) -> FileResponse:
    card = get_card(card_id)
    if not card["image_path"] or not Path(card["image_path"]).exists():
        raise HTTPException(404, "Generate this card image first")
    return FileResponse(
        card["image_path"],
        media_type="image/png",
        filename=image_filename(card["word"]),
    )


@app.post("/api/cards/{card_id}/review")
def review_card(card_id: int) -> dict:
    get_card(card_id)
    with connection() as conn:
        conn.execute(
            "UPDATE cards SET review_count=review_count+1, last_reviewed_at=? WHERE id=?",
            (now(), card_id),
        )
        return serialize(
            conn.execute("SELECT * FROM cards WHERE id=?", (card_id,)).fetchone(), conn
        )


async def send_card_to_epd(card_id: int) -> dict:
    card, endpoint = get_card(card_id), os.getenv("EPD_UPLOAD_URL")
    if not endpoint:
        raise HTTPException(503, "EPD_UPLOAD_URL is not configured")
    if not card["image_path"] or not Path(card["image_path"]).exists():
        raise HTTPException(409, "Generate this card image before uploading")
    try:
        endpoint = endpoint.rstrip("/")
        headers = (
            {"Authorization": f"Bearer {os.getenv('EPD_API_TOKEN')}"}
            if os.getenv("EPD_API_TOKEN")
            else {}
        )
        frame = epd_frame(Path(card["image_path"]))
        async with EPD_SEND_LOCK:
            async with httpx.AsyncClient(timeout=90) as client:
                panel_response = await client.post(
                    f"{endpoint}/api/panel",
                    data={"panelType": EPD_PANEL_TYPE, "colorMode": EPD_COLOR_MODE},
                    headers=headers,
                )
                panel_response.raise_for_status()
                frame_response = await client.post(
                    f"{endpoint}/api/frame",
                    files={"frame": ("frame.bin", frame, "application/octet-stream")},
                    headers=headers,
                )
                frame_response.raise_for_status()
    except (OSError, httpx.HTTPError) as exc:
        raise HTTPException(502, f"EPD gateway error: {exc}") from exc
    return {
        "ok": True,
        "card_id": card_id,
        "endpoint": endpoint,
        "panel_type": EPD_PANEL_TYPE,
        "color_mode": EPD_COLOR_MODE,
        "frame_bytes": len(frame),
    }


@app.post("/api/cards/{card_id}/epd")
async def upload_to_epd(card_id: int) -> dict:
    return await send_card_to_epd(card_id)
