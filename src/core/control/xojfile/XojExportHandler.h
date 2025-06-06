/*
 * Xournal++
 *
 * Export a document for as .xoj compatible for Xournal,
 * remove all additional features which break the compatibility
 *
 * @author Xournal++ Team
 * https://github.com/xournalpp/xournalpp
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include "model/PageRef.h"  // for PageRef

#include "SaveHandler.h"  // for SaveHandler

class AudioElement;
class Stroke;
class XmlAudioNode;
class XmlNode;
class XmlPointNode;


class XojExportHandler: public SaveHandler {
public:
    XojExportHandler();
    virtual ~XojExportHandler();

protected:
    /**
     * Export the fill attributes
     */
    void visitStrokeExtended(XmlPointNode* stroke, const Stroke* s) override;
    void writeHeader() override;
    void writeSolidBackground(XmlNode* background, ConstPageRef p) override;
    void writeTimestamp(XmlAudioNode* xmlAudioNode, const AudioElement* audioElement) override;
    void writeBackgroundName(XmlNode* background, ConstPageRef p) override;

private:
};
