#include "library/media_query.h"

#include "library/db.h"

std::vector<Match> searchMedia(Db& db, const std::wstring& term, int limit,
                               const std::wstring& pathPrefix, int sortCol,
                               bool sortAsc) {
    static const char* kCols[] = {"artist", "title", "genre",
                                  "year",   "bpm",   "duration_ms"};
    if (sortCol < 0 || sortCol > 5) sortCol = 0;
    const std::string folded = utf8(foldW(term));
    std::string sql = // fold() = case- and accent-insensitive ("eglise" finds "Église")
        "SELECT id,artist,title,path,type,duration_ms,genre,year,bpm FROM media_item "
        "WHERE type IN ('audio','mp3g','video','karaoke_zip') AND path LIKE ?3 ";
    if (!folded.empty()) // pre-folded text: one LIKE scan, no fold() calls
        sql += "AND search_f LIKE ?1 ";
    sql += "ORDER BY ";
    sql += kCols[sortCol];
    sql += " COLLATE NOCASE ";
    sql += sortAsc ? "ASC" : "DESC";
    sql += ", artist COLLATE NOCASE, title COLLATE NOCASE LIMIT ?2";

    Db::Stmt q;
    db.prepare(q, sql.c_str());
    if (!folded.empty()) q.bind(1, "%" + folded + "%");
    q.bind(2, int64_t(limit)).bind(3, utf8(pathPrefix) + "%");
    std::vector<Match> out;
    while (q.step()) {
        Match m;
        m.id = q.colInt(0);
        m.artist = wide(q.colText(1));
        m.title = wide(q.colText(2));
        m.label = m.artist.empty() ? m.title : m.artist + L" - " + m.title;
        m.path = wide(q.colText(3));
        m.type = q.colText(4);
        m.durMs = q.colInt(5);
        m.genre = wide(q.colText(6));
        m.year = q.colInt(7);
        m.bpm = q.colInt(8);
        out.push_back(std::move(m));
    }
    return out;
}
