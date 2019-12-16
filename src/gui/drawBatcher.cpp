#include "drawBatcher.hpp"
#include "../globalFunctions.hpp"
#include <math.h>

DrawBatcher drawBatcher;


DrawBatcher::DrawBatcher() :itemCount(0), textCount(0), lineCount(0)
{
	items.setPrimitiveType(sf::Quads);
	lines.setPrimitiveType(sf::Lines);

}

void DrawBatcher::addItemSingleColor(float x, float y, float w, float h, sf::Color color)
{
	items.resize(itemCount+4);
	sf::Vertex *v = &items[itemCount];
	v[0].position.x = x;
	v[0].position.y = y;
	v[1].position.x = x + w;
	v[1].position.y = y;
	v[2].position.x = x + w;
	v[2].position.y = y+h;
	v[3].position.x = x;
	v[3].position.y = y+h;

	v[0].color=color;
	v[1].color=color;
	v[2].color=color;
	v[3].color=color;

	itemCount+=4;
}


void DrawBatcher::addItemBicolor(float x, float y, float w, float h, sf::Color color, sf::Color color2)
{

	items.resize(itemCount+4);
	sf::Vertex *v = &items[itemCount];
	v[0].position.x = x;
	v[0].position.y = y;
	v[1].position.x = x + w;
	v[1].position.y = y;
	v[2].position.x = x + w;
	v[2].position.y = y+h;
	v[3].position.x = x;
	v[3].position.y = y+h;

	v[0].color=color;
	v[1].color=color;
	v[2].color=color2;
	v[3].color=color2;

	itemCount+=4;
}

void DrawBatcher::addItem(Operator *o)
{
	

	addItemSingleColor(o->bg.getPosition().x-1, o->bg.getPosition().y-1, OP_SIZE+2, OP_SIZE+2, o->bg.getOutlineColor());

	addItemSingleColor(o->bg.getPosition().x, o->bg.getPosition().y, OP_SIZE, OP_SIZE, (o->active || o->highlighted || o->hovered ) ? colors[OPERATORBGHOVER] : colors[o->muted ? OPERATORBGMUTED : OPERATORBG]);

	addItemSingleColor(o->bgValue.getPosition().x, o->bgValue.getPosition().y, OP_SIZE, o->bgValue.getSize().y, o->bgValue.getFillColor());
	

	if (o->active || o->highlighted || o->hovered)
	{
		o->number.setColor(colors[o->muted ? OPERATORBGMUTED : OPERATORTEXTHOVER]);
	}
	else
	{
		o->number.setColor(colors[o->muted ? OPERATORTEXTHOVER : OPERATORTEXT]);
	}

	addItem(&o->number);
}

void DrawBatcher::addItem(Button *b)
{
	addItemBicolor(b->bg2[0].position.x, b->bg2[0].position.y, b->w, b->h, b->bg2[0].color, b->bg2[2].color);
	addItem(&b->text);
}

void DrawBatcher::addItem(DataSlider *d)
{
	if (d->visible)
	{
		if (d->focused)
		{
			addItemSingleColor(d->x, d->y, d->width, 19, colors[SLIDEROUTLINE]);
			addItemSingleColor(d->x+1, d->y+1, d->width-2, 17, d->focused ? colors[SLIDERBGHOVER] : colors[SLIDERBG]);
		}
		else
		{
			addItemSingleColor(d->x, d->y, d->width, 19, d->focused ? colors[SLIDERBGHOVER] : colors[SLIDERBG]);
		}
	

		addItemSingleColor(d->bgValue.getPosition().x, d->y, d->bgValue.getSize().x, 19, d->focused ? colors[SLIDERBARHOVER] : colors[SLIDERBAR]);
		addItem(&d->name);

		addItem(&d->tvalue);
	}
}

void DrawBatcher::addItem(MiniVuMeter *v)
{
	addItemSingleColor(v->x, v->y+ 79, v->w, v->h, colors[VUMETERBARLOW]);
}

void DrawBatcher::addItem(TextInput *t)
{
	addItem(&t->bg);
	addItem(&t->text);
	addItem(&t->title);
	addItem(&t->selection);
	if(t->cursorBlink < 32)
		addItem(&t->cursor);
}

void DrawBatcher::addItem(Slider *s)
{
	addItemSingleColor(s->x, s->y, s->bar.getSize().x, s->bar.getSize().y, s->bar.getFillColor());
	addItemSingleColor(s->cursor.getPosition().x, s->cursor.getPosition().y, s->cursor.getSize().x, s->cursor.getSize().y, s->cursor.getFillColor());
}


void DrawBatcher::addItem(sf::VertexArray *v)
{
	items.resize(itemCount+4);

	items[itemCount] = (*v)[0];
	items[itemCount+1] = (*v)[1];
	items[itemCount+2] = (*v)[2];
	items[itemCount+3] = (*v)[3];
	itemCount+=4;
}

void DrawBatcher::addLine(sf::Vertex *l1, sf::Vertex *l2)
{
	lines.resize(lineCount+2);

	lines[lineCount] = *l1;
	lines[lineCount+1] = *l2;
	lineCount+=2;
}


void DrawBatcher::addItem(StereoVuMeter *v)
{
	addItem(&v->vuLeft);
	addItem(&v->vuRight);

	for (int i = 0; i < 5; i++)
	{
		addItem(&v->dbBars[i]);
	}

	for (int i = 0; i < 6; i++)
	{
		addItem(&v->dbValues[i]);
	}
}

void DrawBatcher::addItem(VuMeter *v)
{
	addItem(&v->title);
	addItem(&v->quad);
	addItem(&v->quad2);
	addItem(&v->bar);
	addItem(&v->topbar);
	addItem(&v->db);
}

void DrawBatcher::addItem(List *l)
{
	if (l->selected)
	{
		l->bg.setOutlineColor(colors[FOCUSOUTLINE]);
		addItem(&l->bg);
	}
	else
	{
		l->bg.setOutlineColor(colors[LISTOUTLINE]);
		addItem(&l->bg);
	}

	if (l->pressed)
		addItem(&l->s2);

	if (l->selectionVisible()) {
		
		for (unsigned i = 0; i < l->selecteds_s.size(); i++)
		{
			if ((int)round(l->selecteds_s[i].getPosition().y)>=l->y && (int)round(l->selecteds_s[i].getPosition().y)<l->y+(int)round(l->bg.getSize().y))
				addItem(&l->selecteds_s[i]);
		}
	
	}


	for (unsigned i = l->scroll; i<min<int>(l->scroll + l->maxrows, l->text.size()); ++i)
	{
		l->text[i].setPosition(l->x + l->paddingleft, l->y + (i - l->scroll) * 17);
		addItem(&l->text[i]);
		if (l->pings[i]>0)
		{
			l->squarePing.setPosition(l->x + 4, l->y + (i - l->scroll) * 17 + 5);
			addItem(&l->squarePing);
		}

	}
	if (l->text.size() > l->maxrows)
		addItem(&l->scrollbar);
}


void DrawBatcher::addItem(RectangleShape *r)
{
	if (r->getOutlineThickness() > 0.5f)
	{
		addItemSingleColor(r->getPosition().x-1, r->getPosition().y-1, r->getSize().x+2, r->getSize().y+2, r->getOutlineColor());
		addItemSingleColor(r->getPosition().x, r->getPosition().y, r->getSize().x, r->getSize().y, r->getFillColor());
	}
	else
	{
		addItemSingleColor(r->getPosition().x, r->getPosition().y, r->getSize().x, r->getSize().y, r->getFillColor());
	}
	
}


void DrawBatcher::initialize()
{
	items.resize(0);
	lines.resize(0);
	texts.resize(0);
	itemCount=0;
	textCount=0;
	lineCount=0;
}

void DrawBatcher::draw()
{

	items.resize(itemCount);
	lines.resize(lineCount);
	texts.resize(textCount);

	window->draw(items);
	window->draw(lines);
	for (unsigned i=0; i<textCount; i++)
		window->draw(*texts[i]);
	
}

void DrawBatcher::addItem(sf::Text* text)
{
	texts.resize(textCount+1);
	texts[textCount]=text;
	textCount++;
}
