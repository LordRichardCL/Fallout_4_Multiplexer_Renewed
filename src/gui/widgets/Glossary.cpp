#include "widgets/Glossary.hpp"

#include <QDialog>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QTextBrowser>
#include <QVBoxLayout>

#include <utility>
#include <vector>

namespace gui {

namespace {

const std::vector<std::pair<const char*, const char*>> kTerms = {
    {"Plugin", "A mod's .esp, .esm or .esl file: the records (items, quests, places...) it adds or changes."},
    {"Load order", "The order the game loads plugins in. When two plugins change the same thing, the one loaded later "
                   "wins."},
    {"Master", "A plugin another plugin is built on. A plugin cannot load without its masters, so a plugin that stays "
               "in the load order keeps its masters there too."},
    {"ESM / ESP / ESL", "Plugin kinds. ESM files load first; ESP files are ordinary plugins; ESL (or an ESL-flagged "
                        "plugin) is a light plugin, which does not take one of the game's 254 full plugin slots."},
    {"Light plugin (ESL flag)", "A plugin that shares one load order slot with up to 4,096 others. It can hold at most "
                                "4,096 new records."},
    {"Record", "One thing in a plugin: a weapon, an NPC, a quest, a cell, a keyword..."},
    {"FormID", "A record's ID number. Saves remember records by FormID, which is why compiled records keep theirs "
               "from build to build."},
    {"Override", "A plugin's changed copy of a record another plugin defined. The last override in the load order "
                 "is the one the game uses (the winner)."},
    {"Winner", "The version of a record the game uses: the last plugin in the load order that changes it."},
    {"Conflict", "Two or more plugins change the same record. Simple: only one actually changes something. Hard: "
                 "they change the same value differently."},
    {"ITM", "Identical to master: an override that changes nothing. Harmless, but it can undo another mod's change."},
    {"Compile", "Put the records of many plugins into a few output plugins, with the same game data, so they use "
                "fewer load order slots."},
    {"Output plugins", "The plugins this app writes (Dummy_Plugin_01.esp and so on). They replace the plugins they "
                       "compile, which are then switched off."},
    {"Will compile", "The plugin goes into the outputs and is switched off when you switch over."},
    {"Stays in load order", "The plugin is not compiled: it holds something the compiler cannot reproduce, or a "
                            "plugin that stays needs it as a master. The reason is shown beside it."},
    {"Kept by you", "You excluded the plugin, or it is not among the plugins you chose to compile."},
    {"Switch over / Apply to Game", "Install the outputs and switch the compiled plugins off. Nothing in your game "
                                    "changes before this, and it can be undone."},
    {"Archive (BA2)", "A packed file holding a mod's meshes, textures, scripts or sounds. The game loads it with "
                      "its plugin."},
    {"Loose files", "Mod files outside archives, in the Data folder. A loose file wins over any archive."},
    {"Precombines / previs", "Pre-built meshes and visibility data that make cells render fast. They are tied to the "
                             "plugin that built them."},
    {"FaceGen", "The pre-built faces of NPCs, stored under the plugin's name and the NPC's FormID."},
    {"Patch (Multiplexer_Patch.esp)", "A plugin combining changes of plugins that stay in the load order, like a "
                                      "Bashed Patch."},
    {"Auto-Merger", "An advanced option that also compiles plugins that scripts or configs name by file name, and "
                    "rewrites those files."},
    {"Placeholder", "An empty light plugin with a compiled plugin's name, kept so that plugins listing it as a "
                    "master still load."},
    {"Diagnostic Recorder", "An optional F4SE plugin that writes a log while you play, for the Diagnostics page."},
};

QString html(const QString& filter)
{
    QString out;
    for (const auto& [term, meaning] : kTerms) {
        const QString t = QString::fromUtf8(term), m = QString::fromUtf8(meaning);
        if (!filter.isEmpty() && !t.contains(filter, Qt::CaseInsensitive) && !m.contains(filter, Qt::CaseInsensitive))
            continue;
        out += "<p><b style='color:#3ddc84'>" + t.toHtmlEscaped() + "</b><br>" + m.toHtmlEscaped() + "</p>";
    }
    return out.isEmpty() ? QString("<p style='color:#8aa196'>No term matches.</p>") : out;
}

} // namespace

void showGlossary(QWidget* parent)
{
    QDialog dlg(parent);
    dlg.setWindowTitle("Glossary");
    dlg.resize(560, 620);
    auto* l = new QVBoxLayout(&dlg);
    auto* filter = new QLineEdit;
    filter->setPlaceholderText("Find a word…");
    filter->setClearButtonEnabled(true);
    auto* text = new QTextBrowser;
    text->setHtml(html({}));
    QObject::connect(filter, &QLineEdit::textChanged, text, [text](const QString& f) { text->setHtml(html(f.trimmed())); });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    l->addWidget(filter);
    l->addWidget(text, 1);
    l->addWidget(buttons);
    filter->setFocus();
    dlg.exec();
}

} // namespace gui
