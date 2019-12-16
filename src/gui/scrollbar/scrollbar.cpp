#include "scrollbar.hpp"
#include <math.h>


Slider::Slider(float def, float _max, float _min, float _step, int _x, int _y, int _size, bool _mini, bool _horizontal) :
x(_x),y(_y),size(_size), vmax(_max), step(_step), vmin(_min), mini(_mini), value(def), active(false), delta(0), horizontal(_horizontal)
{

	bar.setPosition(x, y);
	setSize(_size);
	update();
}

void Slider::draw()
{
	window->draw(bar);
	window->draw(cursor);
}
bool Slider::update()
{
	upd_cursor = false;
	if (mouse.pos.x >= bar.getPosition().x && mouse.pos.x < bar.getPosition().x + bar.getSize().x && mouse.pos.y >= bar.getPosition().y && mouse.pos.y < bar.getPosition().y + size)
	{
		bar.setFillColor(colors[SCROLLBARBGHOVER]);
		cursor.setFillColor(colors[SCROLLBARCURSORHOVER]);
		if (mouse.clickg && !active)
		{
			active = true;
			if (horizontal && mouse.pos.x >= cursor.getPosition().x && mouse.pos.x < cursor.getPosition().x + cursor.getSize().x
				||
				!horizontal && mouse.pos.y >= cursor.getPosition().y && mouse.pos.y < cursor.getPosition().y + cursor.getSize().y)
			{

				if (horizontal)
					delta = mouse.pos.x - cursor.getPosition().x;
				else
					delta = mouse.pos.y - cursor.getPosition().y;
			}
			else
			{
				if (horizontal)
					delta = cursor.getSize().x / 2;
				else
					delta = cursor.getSize().y / 2;
			}
		}

	}
	else
	{
		bar.setFillColor(colors[SCROLLBARBG]);
		cursor.setFillColor(colors[SCROLLBARCURSOR]);
	}

	if (active)
	{
		if (horizontal)
			value = ((mouse.pos.x - delta - bar.getPosition().x)) / (size - cursor.getSize().x)*(vmax - vmin) + vmin;
		else
			value = ((mouse.pos.y - delta - bar.getPosition().y)) / (size - cursor.getSize().y)*(vmax - vmin) + vmin;
		upd_cursor = true;
	}
	if (!Mouse::isButtonPressed(Mouse::Left))
	{
		active = false;
	}

	if (upd_cursor)
	{
		cursor.setFillColor(colors[SCROLLBARCURSORCLICKED]);
		value = (int)(value*(1.f / step) + 0.5f);
		value = clamp(value / (1.f / step), vmin, vmax);
		setValue(value);
		return true;
	}
	return false;
}

void Slider::setValue(float _value)
{
	value = clamp(_value, vmin, vmax);
	if (horizontal)
		cursor.setPosition((int)round(bar.getPosition().x + value*(size - cursor.getSize().x) / (vmax - vmin) + (vmin*((size - cursor.getSize().x) / (vmax - vmin)))), bar.getPosition().y);
	else
		cursor.setPosition(bar.getPosition().x, (int)round(bar.getPosition().y + value*(size - cursor.getSize().y) / (vmax - vmin) + (vmin*((size - cursor.getSize().y) / (vmax - vmin)))));
}

void Slider::setSize(int _size)
{
	size = _size;
	if (horizontal)
		bar.setSize(Vector2f(size, mini ? 9 : 16));
	else
		bar.setSize(Vector2f(mini ? 9 : 16, size));

	setValue(value);

}

void Slider::setPosition(int x, int y)
{
	bar.setPosition(x, y);
	setValue(value);
}

void Slider::setScrollableContent(int content, int bounds)
{
	if (horizontal)
		cursor.setSize(Vector2f(content <= bounds ? 0 : size*((float)bounds / content), bar.getSize().y));
	else
		cursor.setSize(Vector2f(bar.getSize().x, content <= bounds ? 0 : size*((float)bounds / content)));
	setValue(value);
}
