// SPDX-License-Identifier: GPL-3.0-or-later
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
	std::cout << "Settings defaults, serialization, restore, path decoding and immutable copies: passed\n";
}
