#include "library/media_query.h"

#include "library/db.h"

std::vector<Match> searchMedia(Db& db, const std::wstring& term, int limit,
                               const std::wstring& pathPrefix, int sortCol,
                               bool sortAsc) {
    static const char* kCols[] = {"artist", "title", "genre", "year"};
    if (sortCol < 0 || sortCol > 3) sortCol = 0;
    std::string sql = // fold() = case- and accent-insensitive ("eglise" finds "Église")
        "SELECT id,artist,title,path,type,duration_ms,genre,year FROM media_item "
        "WHERE (fold(title) LIKE ?1 OR fold(artist) LIKE ?1 OR fold(path) LIKE ?1) "
        "AND type IN ('audio','mp3g','video','karaoke_zip') AND path LIKE ?3 ORDER BY ";
    sql += kCols[sortCol];
    sql += " COLLATE NOCASE ";
    sql += sortAsc ? "ASC" : "DESC";
    sql += ", artist COLLATE NOCASE, title COLLATE NOCASE LIMIT ?2";

    const std::string like = "%" + utf8(foldW(term)) + "%";
    Db::Stmt q;
    db.prepare(q, sql.c_str());
    q.bind(1, like).bind(2, int64_t(limit)).bind(3, utf8(pathPrefix) + "%");
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
        out.push_back(std::move(m));
    }
    return out;
}
