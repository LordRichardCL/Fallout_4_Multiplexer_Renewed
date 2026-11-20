#pragma once

class QWidget;

namespace gui {

// The words the app cannot avoid (plugin, master, override, ESL...), each in plain language, with a filter.
// Opened with F1 or the Glossary link under the navigation.
void showGlossary(QWidget* parent);

} // namespace gui
