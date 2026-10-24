// SPDX-License-Identifier: GPL-3.0-or-later
#include "lighthouse_reference_settings.h"
#include "settings.h"
#include "wivrn_server.h"
#include <QCoreApplication>
#include <QUrl>
#include <iostream>
#include <stdexcept>
// No D-Bus instance or writes in this unit test; production save() remains compiled.
const QMetaObject wivrn_server::staticMetaObject = QObject::staticMetaObject;
void wivrn_server::setJsonConfiguration(QString)
{
	throw std::runtime_error("unexpected D-Bus write in settings unit test");
}
void require(bool condition)
{
	if (!condition)
		throw std::runtime_error("settings assertion failed");
}
int main(int argc, char ** argv)
{
	QCoreApplication app(argc, argv);
	qputenv("XDG_DATA_HOME", "/tmp/lh-xdg-data");
	Settings a;
	a.loadJson("{}");
	require(!a.lighthouseRecordingEnabled());
	require(a.lighthouseRecordingOutput() == "/tmp/lh-xdg-data/wivrn/lighthouse-recordings");
	a.loadJson(R"({"unrelated":42,"lighthouse-recording":{"enabled":false}})");
	a.set_lighthouseRecordingEnabled(true);
	a.set_lighthouseRecordingOutput("/tmp/日本語 with spaces");
	Settings b;
	b.loadJson(a.saveJson());
	require(b.lighthouseRecordingEnabled());
	require(b.lighthouseRecordingOutput() == "/tmp/日本語 with spaces");
	require(b.saveJson().contains("unrelated"));
	a.set_lighthouseRecordingEnabled(false);
	a.set_lighthouseRecordingOutput("/tmp/new-output");
	require(b.lighthouseRecordingEnabled());
	require(b.lighthouseRecordingOutput() == "/tmp/日本語 with spaces");
	b.loadJson(a.saveJson());
	require(!b.lighthouseRecordingEnabled());
	require(b.lighthouseRecordingOutput() == "/tmp/new-output");
	require(b.recordingDirectoryFromUrl(QUrl::fromLocalFile("/tmp/日本語 with spaces")) == "/tmp/日本語 with spaces");
	b.restore_defaults();
	require(!b.lighthouseRecordingEnabled());
	require(b.lighthouseRecordingOutput() == "/tmp/lh-xdg-data/wivrn/lighthouse-recordings");
	b.loadJson(R"({"lighthouse-recording":{"enabled":"invalid","output-directory":null}})");
	require(!b.lighthouseRecordingEnabled());
	require(b.lighthouseRecordingOutput() == "/tmp/lh-xdg-data/wivrn/lighthouse-recordings");
	// The optional backend is opt-in, persists, and resets with defaults.
	a.loadJson(R"({"unrelated":42})");
	require(!a.useSurvive());
	int survive_changes = 0;
	QObject::connect(&a, &Settings::useSurviveChanged, [&] { ++survive_changes; });
	a.set_useSurvive(true);
	a.set_useSurvive(true);
	require(survive_changes == 1);
	b.loadJson(a.saveJson());
	require(b.useSurvive());
	require(b.saveJson().contains("unrelated"));
	a.set_useSurvive(false);
	b.loadJson(a.saveJson());
	require(!b.useSurvive());
	b.set_useSurvive(true);
	b.restore_defaults();
	require(!b.useSurvive());
	require(!b.saveJson().contains("use-survive"));
	b.loadJson(R"({"use-survive":"invalid"})");
	require(!b.useSurvive());
	// Manual selection survives roundtrip; disabling keeps remembered choices.
	a.loadJson("{}");
	require(a.lighthouseReferenceSerial().isEmpty());
	require(a.lighthouseReferenceCandidates().isEmpty());
	a.set_lighthouseReferenceSerial(" LHB-A ");
	a.set_lighthouseReferenceSerial("LHB-B");
	a.set_lighthouseReferenceSerial("");
	b.loadJson(a.saveJson());
	require(b.lighthouseReferenceSerial().isEmpty());
	require(b.lighthouseReferenceCandidates().contains("LHB-A"));
	require(b.lighthouseReferenceCandidates().contains("LHB-B"));
	b.set_lighthouseReferenceSerial("LHB-A");
	a.loadJson(b.saveJson());
	require(a.lighthouseReferenceSerial() == "LHB-A");
	a.set_lighthouseReferenceSerial("invalid serial");
	require(a.lighthouseReferenceSerial() == "LHB-A");
	a.loadJson(R"({"unrelated":42,"lighthouse-reference-candidates":[null,1,"LHB-A","LHB-A","bad serial"]})");
	require(a.lighthouseReferenceCandidates() == QStringList{"LHB-A"});
	// Server discovery merges with the newest JSON and never changes selection.
	auto config = nlohmann::json::parse(a.saveJson().toStdString());
	require(wivrn::remember_lighthouse_reference(config, "LHB-B"));
	require(!wivrn::remember_lighthouse_reference(config, "LHB-B"));
	require(!config.contains("lighthouse-reference-serial"));
	require(config["unrelated"] == 42);
	require(!wivrn::remember_lighthouse_reference(config, "bad serial"));
	a.loadJson(QString::fromStdString(config.dump()));
	require(a.lighthouseReferenceCandidates() == (QStringList{"LHB-A", "LHB-B"}));
	a.restore_defaults();
	require(a.lighthouseReferenceSerial().isEmpty());
	std::cout << "Settings defaults, serialization, restore, path decoding and immutable copies: passed\n";
}
