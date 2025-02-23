/*******************************************************************************
*
* MIT License
*
* Copyright (c) 2017 Advanced Micro Devices, Inc.
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in all
* copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*
*******************************************************************************/

#ifndef GUARD_MIOPEN_FIND_CONTROLS_HPP_
#define GUARD_MIOPEN_FIND_CONTROLS_HPP_

#include <ostream>

namespace miopen {

enum class FindEnforceAction
{
    First_ = 1, // 0 is returned for non-numeric env.vars.
    None   = First_,
    DbUpdate,
    Search,
    SearchDbUpdate,
    DbClean,
    Last_    = DbClean,
    Default_ = None,
};

enum class FindEnforceScope
{
    First_ = 1, // 0 is returned for non-numeric env.vars.
    All    = First_,
    ConvFwd,
    ConvBwd,
    ConvWrW,
    Last_    = ConvWrW,
    Default_ = All,
};

class FindEnforce
{
    FindEnforceAction action;
    FindEnforceScope scope;

    private:
    template <class Context>
    bool IsScopeMatch(const Context& context) const
    {
        if(context.disable_search_enforce)
            return false;
        switch(scope)
        {
        case FindEnforceScope::All: return true;
        case FindEnforceScope::ConvFwd: return context.direction.IsForward();
        case FindEnforceScope::ConvBwd: return context.direction.IsBackwardData();
        case FindEnforceScope::ConvWrW: return context.direction.IsBackwardWrW();
        }
        return false;
    }

    public:
    FindEnforce();

    template <class Context>
    bool IsDbClean(const Context& context) const
    {
        return IsScopeMatch(context) && action == FindEnforceAction::DbClean;
    }

    template <class Context>
    bool IsSearch(const Context& context) const
    {
        return IsScopeMatch(context) &&
               (action == FindEnforceAction::Search || action == FindEnforceAction::SearchDbUpdate);
    }

    template <class Context>
    bool IsDbUpdate(const Context& context) const
    {
        return IsScopeMatch(context) && (action == FindEnforceAction::DbUpdate ||
                                         action == FindEnforceAction::SearchDbUpdate);
    }

    friend std::ostream& operator<<(std::ostream&, const FindEnforce&);
};

} // namespace miopen

#endif // GUARD_MIOPEN_FIND_CONTROLS_HPP_
