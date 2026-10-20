#pragma once
#include "ui/render.h"
namespace lumashot {
// backdrop: optional in-memory copy of the screen over `canvas`; the capture chrome (toolbar,
// panels) blurs it so it looks like the capture toolbar. Without one a flat light canvas is used.
Frame PinAnnotationPreview(const Frame& source,RECT display,RECT canvas,const Frame* backdrop=nullptr);
Document PinAnnotationDisplayDocument(const Document&,const Frame& source,RECT display);
Document PinAnnotationDocument(const Document& display_document,RECT display,const Frame& source);
}