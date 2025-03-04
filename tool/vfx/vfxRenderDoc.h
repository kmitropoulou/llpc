/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2017-2021 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *
 **********************************************************************************************************************/
/**
***********************************************************************************************************************
* @file  vfxRenderDocument.h
* @brief Contains declaration of class RenderDocument
***********************************************************************************************************************
*/

#include "vfxParser.h"

namespace Vfx {

extern void initRenderSections();

// =====================================================================================================================
// Represents the render state result of Vfx parser
class RenderDocument : public Document {
public:
  RenderDocument() {
    initRenderSections();
    memset(&m_renderState, 0, sizeof(m_renderState));
  };

  virtual unsigned getMaxSectionCount(SectionType type);

  virtual VfxRenderStatePtr getDocument();
  bool getPtrOfSubSection(Section *section, unsigned lineNum, const char *memberName, MemberType memberType,
                          bool isWriteAccess, unsigned arrayIndex, Section **ptrOut, std::string *errorMsg);
  Section *createSection(const char *sectionName);

private:
  VfxRenderState m_renderState; // Contains the render state
};

} // namespace Vfx
