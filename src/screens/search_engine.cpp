/***************************************************************************
 *   Copyright (C) 2008-2021 by Andrzej Rybczak                            *
 *   andrzej@rybczak.net                                                   *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.              *
 ***************************************************************************/

#include <array>
#include <boost/range/detail/any_iterator.hpp>
#include <iomanip>

#include "curses/menu_impl.h"
#include "display.h"
#include "global.h"
#include "helpers.h"
#include "screens/playlist.h"
#include "screens/search_engine.h"
#include "settings.h"
#include "status.h"
#include "statusbar.h"
#include "format_impl.h"
#include "helpers/song_iterator_maker.h"
#include "utility/comparators.h"
#include "title.h"
#include "screens/screen_switcher.h"

using Global::MainHeight;
using Global::MainStartY;

namespace ph = std::placeholders;

SearchEngine *mySearcher;

namespace {

std::string SEItemToString(const SEItem &ei);
bool SEItemEntryMatcher(const Regex::Regex &rx,
                        const NC::Menu<SEItem>::Item &item,
                        bool filter);

}

template <>
struct SongPropertiesExtractor<SEItem>
{
	template <typename ItemT>
	auto &operator()(ItemT &item) const
	{
		auto s = !item.isSeparator() && item.value().isSong()
			? &item.value().song()
			: nullptr;
		return m_cache.assign(&item.properties(), s);
	}

private:
	mutable SongProperties m_cache;
};

SongIterator SearchEngineWindow::currentS()
{
	return makeSongIterator(current());
}

ConstSongIterator SearchEngineWindow::currentS() const
{
	return makeConstSongIterator(current());
}

SongIterator SearchEngineWindow::beginS()
{
	return makeSongIterator(begin());
}

ConstSongIterator SearchEngineWindow::beginS() const
{
	return makeConstSongIterator(begin());
}

SongIterator SearchEngineWindow::endS()
{
	return makeSongIterator(end());
}

ConstSongIterator SearchEngineWindow::endS() const
{
	return makeConstSongIterator(end());
}

std::vector<MPD::Song> SearchEngineWindow::getSelectedSongs()
{
	std::vector<MPD::Song> result;
	for (auto &item : *this)
	{
		if (item.isSelected())
		{
			assert(item.value().isSong());
			result.push_back(item.value().song());
		}
	}
	if (result.empty() && !empty() && current()->value().isSong())
		result.push_back(current()->value().song());
	return result;
}

/**********************************************************************/

SearchEngine::SearchEngine()
: Screen(NC::Menu<SEItem>(0, MainStartY, COLS, MainHeight, "", Config.main_color, NC::Border()))
{
	setHighlightFixes(w);
	w.cyclicScrolling(Config.use_cyclic_scrolling);
	w.centeredCursor(Config.centered_cursor);
	w.setItemDisplayer(std::bind(Display::SEItems, ph::_1, std::cref(w)));
	w.setSelectedPrefix(Config.selected_item_prefix);
	w.setSelectedSuffix(Config.selected_item_suffix);
}

void SearchEngine::resize()
{
	size_t x_offset, width;
	getWindowResizeParams(x_offset, width);
	w.resize(width, MainHeight);
	w.moveTo(x_offset, MainStartY);
	switch (Config.search_engine_display_mode)
	{
		case DisplayMode::Columns:
			if (Config.titles_visibility)
				w.setTitle(Display::Columns(w.getWidth()));
			break;
		case DisplayMode::Classic:
			w.setTitle("");
			break;
	}
	hasToBeResized = 0;
}

void SearchEngine::switchTo()
{
	SwitchTo::execute(this);
	drawHeader();
}

std::wstring SearchEngine::title()
{
	return L"Search engine";
}

void SearchEngine::mouseButtonPressed(MEVENT me)
{
	if (w.empty() || !w.hasCoords(me.x, me.y) || size_t(me.y) >= w.size())
		return;
	if (me.bstate & (BUTTON1_PRESSED | BUTTON3_PRESSED))
	{
		if (!w.Goto(me.y))
			return;
		w.refresh();
		if (w.current()->value().isSong())
		{
			bool play = me.bstate & BUTTON3_PRESSED;
			addItemToPlaylist(play);
		}
	}
	else
		Screen<WindowType>::mouseButtonPressed(me);
}

/***********************************************************************/

bool SearchEngine::allowsSearching()
{
	return !w.empty();
}

const std::string &SearchEngine::searchConstraint()
{
	return m_search_predicate.constraint();
}

void SearchEngine::setSearchConstraint(const std::string &constraint)
{
	m_search_predicate = Regex::ItemFilter<SEItem>(
		constraint,
		Config.regex_type,
		std::bind(SEItemEntryMatcher, ph::_1, ph::_2, false));
}

void SearchEngine::clearSearchConstraint()
{
	m_search_predicate.clear();
}

bool SearchEngine::search(SearchDirection direction, bool wrap, bool skip_current)
{
	return ::search(w, m_search_predicate, direction, wrap, skip_current);
}

/***********************************************************************/

bool SearchEngine::allowsFiltering()
{
	return allowsSearching();
}

std::string SearchEngine::currentFilter()
{
	std::string result;
	if (auto pred = w.filterPredicate<Regex::ItemFilter<SEItem>>())
		result = pred->constraint();
	return result;
}

void SearchEngine::applyFilter(const std::string &constraint)
{
	if (!constraint.empty())
	{
		w.applyFilter(Regex::ItemFilter<SEItem>(
			              constraint,
			              Config.regex_type,
			              std::bind(SEItemEntryMatcher, ph::_1, ph::_2, true)));
	}
	else
		w.clearFilter();
}

/***********************************************************************/

bool SearchEngine::actionRunnable()
{
	return true;
}

void SearchEngine::runAction()
{
	if (w.empty() || !w.current()->value().isSong())
		openSearchPrompt();
	else
		addItemToPlaylist(true);
}

/***********************************************************************/

bool SearchEngine::itemAvailable()
{
	return !w.empty() && w.current()->value().isSong();
}

bool SearchEngine::addItemToPlaylist(bool play)
{
	return addSongToPlaylist(w.current()->value().song(), play);
}

std::vector<MPD::Song> SearchEngine::getSelectedSongs()
{
	return w.getSelectedSongs();
}

/***********************************************************************/

void SearchEngine::searchDatabase(const std::string &query)
{
	m_last_query = query;
	w.clearFilter();
	w.clear();

	if (query.empty())
		return;

	Mpd.StartSearch(false);
	Mpd.AddSearchAny(query);
	for (MPD::SongIterator s = Mpd.CommitSearchSongs(), end; s != end; ++s)
		w.addItem(std::move(*s));

	if (Config.search_engine_display_mode == DisplayMode::Columns && Config.titles_visibility)
		w.setTitle(Display::Columns(w.getWidth()));
}

void SearchEngine::openSearchPrompt()
{
	using Global::wFooter;

	Statusbar::ScopedLock slock;
	Statusbar::put() << NC::Format::Bold << "Search(y): " << NC::Format::NoBold;

	NC::Window::ScopedPromptHook helper(*wFooter,
		Statusbar::Helpers::SearchDatabaseImmediately(this));

	std::string query;
	try {
		query = wFooter->prompt(m_last_query);
	} catch (NC::PromptAborted &) {
		return;
	}

	m_last_query = query;

	if (!w.empty())
	{
		size_t found = w.size();
		Statusbar::printf("Found %1% %2%", found, found == 1 ? "song" : "songs");
	}
	else if (!query.empty())
		Statusbar::print("No results found");
}

void SearchEngine::reset()
{
	m_last_query.clear();
	w.clearFilter();
	w.clear();
	w.setTitle("");
	Statusbar::print("Search state reset");
}

/***********************************************************************/

namespace {

std::string SEItemToString(const SEItem &ei)
{
	std::string result;
	if (ei.isSong())
	{
		switch (Config.search_engine_display_mode)
		{
			case DisplayMode::Classic:
				result = Format::stringify<char>(Config.song_list_format, &ei.song());
				break;
			case DisplayMode::Columns:
				result = Format::stringify<char>(Config.song_columns_mode_format, &ei.song());
				break;
		}
	}
	else
		result = ei.buffer().str();
	return result;
}

bool SEItemEntryMatcher(const Regex::Regex &rx, const NC::Menu<SEItem>::Item &item, bool filter)
{
	if (item.isSeparator() || !item.value().isSong())
		return filter;
	return Regex::search(SEItemToString(item.value()), rx, Config.ignore_diacritics);
}

}
