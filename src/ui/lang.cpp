#include "ui/im.h"

#include <unordered_map>

// English -> French UI dictionary. Translation happens centrally (Ui::text
// and the context-menu builder), so draw sites keep plain English literals.
// Composed strings are handled by the rules in uiTr(): exact match first,
// then "HEAD (tail)" / "HEAD: tail" / "HEAD — tail" retranslate the head.
static int g_lang = 0;

void uiSetLanguage(int lang) { g_lang = lang; }
int uiLanguage() { return g_lang; }

static const std::unordered_map<std::wstring, std::wstring>& frMap() {
    static const std::unordered_map<std::wstring, std::wstring> m = {
        // header / global
        {L"SETTINGS", L"PARAMÈTRES"},
        {L"REQUESTS", L"DEMANDES"},
        {L"VIDEO OUT", L"SORTIE VIDÉO"},
        {L"VIDEO OUT: OFF", L"SORTIE VIDÉO: NON"},
        {L"ready", L"prêt"},
        // decks / mixer
        {L"— drop a track —", L"— déposez une piste —"},
        {L"PAUSE", L"PAUSE"},
        {L"CLEAR", L"VIDER"},
        {L"RESUME", L"REPRENDRE"},
        {L"PLAYING", L"LECTURE"},
        {L"PAUSED", L"EN PAUSE"},
        {L"READY", L"PRÊT"},
        {L"LOADING", L"CHARGEMENT"},
        {L"ERROR", L"ERREUR"},
        {L"CUED — NEXT", L"PRÊTE — SUIVANTE"},
        {L"KEY", L"TONALITÉ"},
        {L"AUTOMIX: CUT", L"AUTOMIX: COUPURE"},
        {L"AUTOMIX: FADE", L"AUTOMIX: FONDU"},
        {L"AUTOMIX: SMART", L"AUTOMIX: INTELLIGENT"},
        // browser
        {L"SEARCH", L"RECHERCHE"},
        {L"FOLDER", L"DOSSIER"},
        {L"TITLE", L"TITRE"},
        {L"ARTIST", L"ARTISTE"},
        {L"GENRE", L"GENRE"},
        {L"YEAR", L"ANNÉE"},
        {L"BPM", L"BPM"},
        {L"Key", L"Tonalité"},
        {L"TIME", L"DURÉE"},
        {L"+ QUEUE", L"+ FILE"},
        {L"MIX ▸", L"MIX ▸"},
        {L"QUEUE", L"FILE"},
        {L"SINGER", L"CHANTEUR"},
        {L"NEXT SINGER ▸", L"SUIVANT ▸"},
        {L"PLAY ▸", L"JOUER ▸"},
        {L"YES", L"OUI"},
        {L"CREATE", L"CRÉER"},
        {L"next up on deck A", L"à suivre platine A"},
        {L"next up on deck B", L"à suivre platine B"},
        {L"fill it by right-click → Add to playlist, or drag rows in",
         L"remplissez par clic droit → Ajouter à une liste, ou glissez des pistes"},
        {L"type a name: their upcoming songs + full history",
         L"tapez un nom : ses chansons à venir + tout son historique"},
        {L"upcoming on top, everything they sang below",
         L"à venir en haut, tout ce qu'il a chanté en dessous"},
        {L"PLAYED TONIGHT", L"JOUÉES CE SOIR"},
        {L"double-click to play again", L"double-cliquez pour rejouer"},
        {L"HISTORY — done tonight", L"HISTORIQUE — terminé ce soir"},
        {L"HISTORY — songs sung", L"HISTORIQUE — chansons chantées"},
        {L"UP NEXT", L"À SUIVRE"},
        // sidebar
        {L"BROWSE", L"PARCOURIR"},
        {L"All tracks", L"Toutes les pistes"},
        {L"Played tonight", L"Jouées ce soir"},
        {L"PLAYLISTS", L"LISTES DE LECTURE"},
        {L"+ New playlist", L"+ Nouvelle liste"},
        {L"FOLDERS", L"DOSSIERS"},
        {L"SINGERS", L"CHANTEURS"},
        {L"Rotation", L"Rotation"},
        {L"+ IMPORT FOLDER", L"+ IMPORTER UN DOSSIER"},
        // singer statuses (raw db values drawn in lists)
        {L"waiting", L"en attente"},
        {L"singing", L"chante"},
        {L"completed", L"terminé"},
        {L"skipped", L"sauté"},
        {L"noshow", L"absent"},
        {L"played", L"joué"},
        // queue drawer
        {L"drag tracks here", L"glissez des pistes ici"},
        // prompts / modals
        {L"CLOSE KARAOKE DJ?", L"FERMER KARAOKE DJ ?"},
        {L"Decks, queue, singer rotation and tonight's history will be cleared.",
         L"Platines, file, rotation des chanteurs et historique du soir seront vidés."},
        {L"Your library, playlists, markers and settings are kept.",
         L"Bibliothèque, listes, repères et paramètres sont conservés."},
        {L"FULL SCREEN", L"PLEIN ÉCRAN"},
        {L"NEW PLAYLIST", L"NOUVELLE LISTE"},
        {L"ADD TO ROTATION — NEW SINGER", L"AJOUT À LA ROTATION — NOUVEAU CHANTEUR"},
        {L"CANCEL", L"ANNULER"},
        {L"DONE", L"TERMINÉ"},
        {L"SAVE", L"ENREGISTRER"},
        {L"SAVE + WRITE FILE", L"ENREGISTRER + FICHIER"},
        {L"BROWSER COLUMNS", L"COLONNES DU NAVIGATEUR"},
        {L"SHOWN", L"VISIBLE"},
        {L"HIDDEN", L"MASQUÉ"},
        {L"PHONE REQUESTS", L"DEMANDES PAR TÉLÉPHONE"},
        {L"YOUTUBE", L"YOUTUBE"},
        {L"Download folder", L"Dossier de téléchargement"},
        {L"PICK FOLDER…", L"CHOISIR UN DOSSIER…"},
        {L"default — %APPDATA%\\KaraokeDJ\\youtube (not in the library)",
         L"par défaut — %APPDATA%\\KaraokeDJ\\youtube (hors bibliothèque)"},
        {L"No pending requests.", L"Aucune demande en attente."},
        {L"ADD", L"AJOUTER"},
        {L"REJECT", L"REFUSER"},
        {L"more waiting", L"autres en attente"},
        {L"EDIT TAGS", L"MODIFIER LES TAGS"},
        {L"Artist", L"Artiste"},
        {L"Title", L"Titre"},
        {L"Genre", L"Genre"},
        {L"Year", L"Année"},
        // settings — sections
        {L"BEHAVIOUR", L"COMPORTEMENT"},
        {L"VIDEO OUTPUT", L"SORTIE VIDÉO"},
        {L"AUDIO OUTPUT", L"SORTIE AUDIO"},
        {L"LIBRARY SCAN", L"ANALYSE DE LA BIBLIOTHÈQUE"},
        {L"WAITING SCREEN", L"ÉCRAN D'ATTENTE"},
        {L"WAITING SCREEN  ▾", L"ÉCRAN D'ATTENTE  ▾"},
        {L"WAITING SCREEN  ▸   (click to design)",
         L"ÉCRAN D'ATTENTE  ▸   (cliquez pour concevoir)"},
        // settings — rows
        {L"Language", L"Langue"},
        {L"ENGLISH", L"ENGLISH"},
        {L"FRANÇAIS", L"FRANÇAIS"},
        {L"AUTO GAIN (LEVEL TRACKS)", L"GAIN AUTO (NIVELER LES PISTES)"},
        {L"trims every track toward the same loudness",
         L"ramène chaque piste vers le même volume"},
        {L"CHECK FOR UPDATES", L"VÉRIFIER LES MISES À JOUR"},
        {L"GET UPDATE", L"OBTENIR LA MISE À JOUR"},
        {L"checking…", L"vérification…"},
        {L"this is v" , L"version v"},
        {L"opens the download page — this is v",
         L"ouvre la page de téléchargement — version v"},
        {L"Off  (no fullscreen output)", L"Non  (pas de sortie plein écran)"},
        {L"Video fit", L"Cadrage vidéo"},
        {L"FIT", L"AJUSTER"},
        {L"FILL", L"REMPLIR"},
        {L"STRETCH", L"ÉTIRER"},
        {L"keep ratio, black bars", L"ratio conservé, bandes noires"},
        {L"keep ratio, crop to fill", L"ratio conservé, recadré"},
        {L"ignore ratio, fill screen", L"ratio ignoré, plein écran"},
        {L"System default  (follows Windows)", L"Défaut système  (suit Windows)"},
        {L"READ FILE TAGS ON IMPORT", L"LIRE LES TAGS À L'IMPORTATION"},
        {L"slower — real titles, artists, durations",
         L"plus lent — vrais titres, artistes, durées"},
        {L"fastest — filenames only; tags fill in on a later import",
         L"le plus rapide — noms de fichiers; les tags viendront plus tard"},
        {L"UPDATE ALL FOLDERS", L"MAJ DE TOUS LES DOSSIERS"},
        {L"rescan every imported folder — unchanged files skip fast",
         L"ré-analyse chaque dossier importé — les fichiers inchangés passent vite"},
        {L"WATCH FOLDERS (AUTO-UPDATE)", L"SURVEILLER LES DOSSIERS (AUTO)"},
        {L"new/changed files import themselves shortly after they appear",
         L"les fichiers nouveaux/modifiés s'importent peu après leur apparition"},
        {L"CLEAN MISSING FILES", L"NETTOYER LES MANQUANTS"},
        {L"drop entries whose file was deleted (unplugged drives are left alone)",
         L"retire les entrées dont le fichier est supprimé (disques débranchés épargnés)"},
        {L"ALLOW PHONE REQUESTS", L"AUTORISER LES DEMANDES PAR TÉLÉPHONE"},
        {L"singers search and request from their phones (same Wi-Fi / hotspot)",
         L"les chanteurs cherchent et demandent depuis leur téléphone (même Wi-Fi)"},
        {L"Password (optional)", L"Mot de passe (optionnel)"},
        {L"phones enter it once; blank = open",
         L"saisi une seule fois par téléphone; vide = ouvert"},
        {L"Phones open:", L"Téléphones :"},
        {L"No network found — join Wi-Fi or start a mobile hotspot",
         L"Aucun réseau — joignez un Wi-Fi ou créez un point d'accès"},
        // waiting screen designer
        {L"the big headline between songs", L"le grand titre entre les chansons"},
        {L"Message", L"Message"},
        {L"free text — venue name, drink specials, anything",
         L"texte libre — nom du bar, promos, ce que vous voulez"},
        {L"Logo", L"Logo"},
        {L"Background", L"Arrière-plan"},
        {L"PICK IMAGE…", L"CHOISIR UNE IMAGE…"},
        {L"none — png/jpg, transparency kept",
         L"aucun — png/jpg, transparence conservée"},
        {L"none — fills the screen, dimmed so text stays readable",
         L"aucun — remplit l'écran, assombri pour garder le texte lisible"},
        {L"show · position (3×3) · size", L"visible · position (3×3) · taille"},
        {L"Next up", L"À suivre"},
        {L"Singer list", L"Liste des chanteurs"},
        {L"QR code", L"Code QR"},
        {L"PREVIEW", L"APERÇU"},
        {L"NEXT UP:", L"À SUIVRE :"},
        {L"SCAN TO REQUEST A SONG", L"SCANNEZ POUR DEMANDER UNE CHANSON"},
        // common statuses (exact or head-translated)
        {L"import complete", L"importation terminée"},
        {L"importing", L"importation"},
        {L"playing", L"lecture"},
        {L"loading", L"chargement"},
        {L"queued", L"en file"},
        {L"queued next", L"suivant dans la file"},
        {L"NOW SINGING", L"CHANTE MAINTENANT"},
        {L"rotation cleared — ready for a new night",
         L"rotation vidée — prêt pour une nouvelle soirée"},
        {L"tonight's history cleared", L"historique de ce soir effacé"},
        {L"playlist deleted", L"liste supprimée"},
        {L"you're on the latest version", L"vous avez la dernière version"},
        {L"update available", L"mise à jour disponible"},
        {L"phone requests on", L"demandes par téléphone activées"},
        {L"phone requests off", L"demandes par téléphone désactivées"},
        {L"phone request", L"demande reçue"},
        {L"rotation", L"rotation"},
        {L"tags saved", L"tags enregistrés"},
        {L"tags saved to library and written into the file",
         L"tags enregistrés dans la bibliothèque et écrits dans le fichier"},
        {L"watching library folders for changes",
         L"surveillance des dossiers de la bibliothèque activée"},
        {L"folder watching off", L"surveillance des dossiers désactivée"},
        {L"folder changed — updating library",
         L"dossier modifié — mise à jour de la bibliothèque"},
        {L"BPM analysis complete", L"analyse BPM terminée"},
        {L"database busy — last change may not have saved",
         L"base de données occupée — le dernier changement n'est peut-être pas enregistré"},
        {L"database busy", L"base de données occupée"},
        {L"ANALYZING BPM + KEY", L"ANALYSE BPM + TONALITÉ"},
        {L"waiting-screen logo set", L"logo de l'écran d'attente défini"},
        {L"waiting-screen background set",
         L"arrière-plan de l'écran d'attente défini"},
        {L"couldn't read that image", L"impossible de lire cette image"},
        // context menus
        {L"Mix now", L"Mixer maintenant"},
        {L"Add to queue", L"Ajouter à la file"},
        {L"Play next (queue front)", L"Jouer ensuite (début de file)"},
        {L"Edit tags…", L"Modifier les tags…"},
        {L"Add to rotation", L"Ajouter à la rotation"},
        {L"Add to playlist", L"Ajouter à une liste"},
        {L"New singer…", L"Nouveau chanteur…"},
        {L"New playlist…", L"Nouvelle liste…"},
        {L"Remove from queue", L"Retirer de la file"},
        {L"Remove from playlist", L"Retirer de la liste"},
        {L"Update library (rescan folder)",
         L"Mettre à jour la bibliothèque (ré-analyser)"},
        {L"Remove from library", L"Retirer de la bibliothèque"},
        {L"Folder moved — relocate…", L"Dossier déplacé — relocaliser…"},
        {L"Sing now", L"Chanter maintenant"},
        {L"Mark completed", L"Marquer terminé"},
        {L"Skip", L"Sauter"},
        {L"No show", L"Absent"},
        {L"Delete playlist", L"Supprimer la liste"},
        {L"Clear rotation (new night)", L"Vider la rotation (nouvelle soirée)"},
        {L"Clear tonight's history", L"Effacer l'historique de ce soir"},
        {L"Set start marker here", L"Poser le marqueur de début ici"},
        {L"Set end marker here", L"Poser le marqueur de fin ici"},
        {L"Clear markers", L"Effacer les marqueurs"},
        {L"Off", L"Non"},
        {L"Excluded", L"Exclues"},
        {L"Exclude from search", L"Exclure de la recherche"},
        {L"Restore to search", L"Restaurer dans la recherche"},
        {L"excluded from search", L"exclue de la recherche"},
        {L"restored to search", L"restaurée dans la recherche"},
        {L"CLEAR THE QUEUE?", L"VIDER LA FILE ?"},
        {L"queued track(s) will be removed.",
         L"piste(s) en file seront retirées."},
        {L"Decks and the rotation are not touched.",
         L"Les platines et la rotation ne sont pas touchées."},
        {L"queue cleared", L"file vidée"},
        {L"Fade transition", L"Transition en fondu"},
        {L"Smart  (skip/detect silence)", L"Intelligent  (saute les silences)"},
    };
    return m;
}

std::wstring uiTr(const std::wstring& s) {
    if (g_lang == 0 || s.empty()) return s;
    const auto& m = frMap();
    const auto it = m.find(s);
    if (it != m.end()) return it->second;
    // "HEAD (tail)" — counters like "All tracks (7215)"
    if (s.back() == L')') {
        const size_t p = s.rfind(L" (");
        if (p != std::wstring::npos) {
            const auto h = m.find(s.substr(0, p));
            if (h != m.end()) return h->second + s.substr(p);
        }
    }
    // "HEAD ▲" / "HEAD ▼" — sort-arrow column headers
    for (const wchar_t* suf : {L" ▲", L" ▼"}) {
        if (s.size() > 2 && s.compare(s.size() - 2, 2, suf) == 0) {
            const auto h = m.find(s.substr(0, s.size() - 2));
            if (h != m.end()) return h->second + suf;
        }
    }
    // "HEAD: tail" / "HEAD — tail" — composed statuses
    for (const wchar_t* sep : {L": ", L" — ", L":  "}) {
        const size_t p = s.find(sep);
        if (p != std::wstring::npos) {
            const auto h = m.find(s.substr(0, p));
            if (h != m.end()) return h->second + s.substr(p);
        }
    }
    return s;
}
