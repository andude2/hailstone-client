// The conversation journal's data (game/conversation_journal.h).
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "game/conversation_journal.h"
#include "scratch_dir.h"

using namespace hailstone;
namespace fs = std::filesystem;

TEST_CASE("Conversation journal: NPCs by name and zone, an entry per text, categories, the files") {
    game::ConversationJournal j;
    j.add("Guard Gehnus", 2, 100, 1.5f, 2.0f, 3.0f, "Guard Gehnus says 'Hail'", -1);
    j.add("Guard Gehnus", 2, 200, 4.0f, 5.0f, 6.0f, "Guard Gehnus says 'Hail'", -1); // the same text: moved
    j.add("Guard Gehnus", 2, 150, 7.0f, 8.0f, 9.0f, "Guard Gehnus says 'Go away'", -1);
    j.add("Guard Gehnus", 3, 50, 0, 0, 0, "Guard Gehnus says 'Hail'", -1); // another zone: another NPC
    REQUIRE(j.npcs().size() == 2);
    const auto& n = *j.npcs()[0];
    REQUIRE(n.entries.size() == 2);
    CHECK(n.entries[0]->time == 200);
    CHECK(n.entries[0]->x == 4);
    CHECK(n.latest == 200);
    CHECK(j.text(n.entries[1]->text) == "Guard Gehnus says 'Go away'");
    CHECK(j.find_text(n, game::journal_upper("go away"), 0) == 1);
    CHECK(j.find_text(n, "NOTHING", 0) == -1);

    auto* quests = j.set_category("Quests", "Things to do", -1);
    auto* lore = j.set_category("Lore", "", -1);
    CHECK(quests->id == 0);
    CHECK(lore->id == 1);
    j.npcs()[0]->entries[1]->category = lore->id;
    j.remove_category(0);
    CHECK(j.set_category("Again", "", -1)->id == 0); // the lowest free id

    hailstone::test::ScratchDir dir_scratch("journal_test");
    const fs::path& dir = dir_scratch.path();
    REQUIRE(j.save(dir / "CJ") == 0);
    std::ifstream hdr(dir / "CJ.hdr");
    std::stringstream ss;
    ss << hdr.rdbuf();
    hdr.close(); // Windows will not remove a file that is still open (below)
    CHECK(ss.str().rfind("EQJOURNAL\n+CAT\n1\nLore\n\n-CAT\n+CAT\n0\nAgain\n\n-CAT\n+NPC\nGuard Gehnus\n2\n-NPC\n+ENTRY\n000000C8\n4\n5\n6\n-1\n", 0) == 0);

    game::ConversationJournal k;
    REQUIRE(k.load(dir / "CJ") == 0);
    REQUIRE(k.npcs().size() == 2);
    CHECK(k.npcs()[0]->entries.size() == 2);
    CHECK(k.text(k.npcs()[0]->entries[1]->text) == "Guard Gehnus says 'Go away'");
    CHECK(k.npcs()[0]->entries[1]->category == 1);
    // read back as the original does (0x47ac60): "Lore" (id 1, none yet) takes the lowest free id, 0, and the
    // next category, id 0, then finds it and renames it
    REQUIRE(k.categories().size() == 1);
    CHECK(k.categories()[0]->name == "Again");
    CHECK(k.categories()[0]->id == 0);
    CHECK(k.load(dir / "missing") == -1);
    fs::remove(dir / "CJ.hdr");
    CHECK(k.load(dir / "CJ") == -2);
    CHECK(k.remove_npc("Guard Gehnus", 3));
    fs::remove_all(dir);
}

TEST_CASE("Conversation journal: the files open in a folder whose name is outside the ANSI code page") {
    // The data folder sits under the user's profile, which may be named in any script; on Windows path::string()
    // throws for such a name and a narrow fopen cannot open it.
    hailstone::test::ScratchDir dir_scratch("journal_unicode_test");
    const fs::path dir = dir_scratch.path() / fs::path(std::u8string(u8"Журнал 日記"));
    fs::create_directories(dir);
    game::ConversationJournal j;
    j.add("Guard Gehnus", 2, 100, 1.0f, 2.0f, 3.0f, "Guard Gehnus says 'Hail'", -1);
    REQUIRE(j.save(dir / "CJ") == 0);
    CHECK(fs::exists(dir / "CJ.txt"));
    CHECK(fs::exists(dir / "CJ.hdr"));
    game::ConversationJournal k;
    REQUIRE(k.load(dir / "CJ") == 0);
    REQUIRE(k.npcs().size() == 1);
    CHECK(k.text(k.npcs()[0]->entries[0]->text) == "Guard Gehnus says 'Hail'");
}

TEST_CASE("Conversation journal: new lines in names and dialogue round-trip as spaces; long names find their NPC") {
    hailstone::test::ScratchDir dir_scratch("journal_newline_test");
    const fs::path& dir = dir_scratch.path();
    game::ConversationJournal j;
    // a name that tries to add records to CJ.hdr, and a line that would split in CJ.txt
    j.add("Bad\n+ENTRY\n00000001\n0\n0\n0\n-1\n00000000\n-ENTRY", 2, 100, 0, 0, 0, "Bad says 'one\r\ntwo'", -1);
    REQUIRE(j.npcs().size() == 1);
    CHECK(j.npcs()[0]->name.find('\n') == std::string::npos);
    CHECK(j.text(j.npcs()[0]->entries[0]->text) == "Bad says 'one  two'");
    const std::string long_name(0x50, 'a');
    j.add(long_name, 2, 100, 0, 0, 0, "Hail", -1);
    j.add(long_name, 2, 110, 0, 0, 0, "Hail again", -1);
    REQUIRE(j.npcs().size() == 2); // the second add finds the truncated NPC
    CHECK(j.npcs()[1]->name.size() == 0x3f);
    CHECK(j.npcs()[1]->entries.size() == 2);
    CHECK(j.npc(long_name, 2, false) == j.npcs()[1].get());
    REQUIRE(j.save(dir / "CJ") == 0);

    game::ConversationJournal k;
    REQUIRE(k.load(dir / "CJ") == 0);
    REQUIRE(k.npcs().size() == 2);
    REQUIRE(k.npcs()[0]->entries.size() == 1);
    CHECK(k.npcs()[0]->name == j.npcs()[0]->name);
    CHECK(k.text(k.npcs()[0]->entries[0]->text) == "Bad says 'one  two'");
    CHECK(k.npcs()[1]->entries.size() == 2);
    CHECK(k.remove_npc(long_name, 2));
}
