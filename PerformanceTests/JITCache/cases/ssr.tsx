// Server-side rendering, the cost that makes frameworks' cold starts slow: React 19's renderToString of a shop's
// category page, with a layout, a navigation bar and a grid of 60 product cards built from nested components with
// lists, keys and conditionals. One pass renders the page of each of 10 categories, whose products all differ.

import { createContext, useContext, type ReactNode } from "react";
import { renderToString } from "react-dom/server";
import type { Workload } from "../run.ts";

// mulberry32: every process draws the same catalog.
function generator(seed: number): () => number {
  let state = seed >>> 0;
  return () => {
    state = (state + 0x6d2b79f5) >>> 0;
    let t = state;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

interface Product {
  id: string;
  name: string;
  brand: string;
  priceCents: number;
  saleCents: number | null;
  rating: number;
  reviews: number;
  badges: string[];
  colors: string[];
  stock: number;
  freeShipping: boolean;
}

interface Category {
  slug: string;
  name: string;
  products: Product[];
}

const categoryNames = ["Running shoes", "Rain jackets", "Backpacks", "Tents", "Sleeping bags", "Headlamps", "Trail poles", "Water bottles", "Base layers", "Camp stoves"];
const itemNouns = ["Runner", "Shell", "Pack", "Tent", "Bag", "Headlamp", "Poles", "Bottle", "Crew", "Stove"];
const brands = ["Alder", "Basalt", "Cirrus", "Dune", "Ember", "Fjord", "Granite", "Harbor"];
const adjectives = ["Ultralight", "All-weather", "Packable", "Insulated", "Breathable", "Reflective", "Recycled", "Convertible"];
const models = ["Ridge", "Summit", "Canyon", "Meadow", "Glacier", "Tundra", "Coast", "Prairie", "Delta", "Mesa"];
const palette = ["#1f2937", "#b91c1c", "#047857", "#1d4ed8", "#a16207", "#6d28d9", "#be185d", "#0f766e"];
const badgeLabels: Record<string, string> = { new: "New", bestseller: "Best seller", eco: "Recycled materials", limited: "Limited edition" };
const productsPerPage = 60;
const shop = { name: "Example Outfitters", currency: "$", freeShippingFromCents: 7500 };
const ShopContext = createContext(shop);

function catalog(): Category[] {
  const random = generator(0xc0ffee);
  const pick = <T,>(list: readonly T[]) => list[Math.floor(random() * list.length)];
  return categoryNames.map((name, c) => ({
    slug: name.toLowerCase().replaceAll(" ", "-"),
    name,
    products: Array.from({ length: productsPerPage }, (_, i) => {
      const priceCents = 1999 + Math.floor(random() * 230) * 100;
      return {
        id: `${c}-${String(i).padStart(3, "0")}`,
        name: `${pick(adjectives)} ${pick(models)} ${itemNouns[c]}`,
        brand: pick(brands),
        priceCents,
        saleCents: random() < 0.3 ? Math.round(priceCents * (0.6 + random() * 0.3)) : null,
        rating: Math.round(random() * 40 + 10) / 10,
        reviews: random() < 0.1 ? 0 : Math.floor(random() * 2500),
        badges: Object.keys(badgeLabels).filter(() => random() < 0.2),
        colors: palette.filter(() => random() < 0.35),
        stock: random() < 0.1 ? 0 : Math.floor(random() * 40) + 1,
        freeShipping: priceCents >= shop.freeShippingFromCents || random() < 0.2,
      };
    }),
  }));
}

function money(currency: string, cents: number): string {
  return `${currency}${(cents / 100).toFixed(2)}`;
}

function Layout({ title, description, children }: { title: string; description: string; children: ReactNode }) {
  const { name } = useContext(ShopContext);
  return (
    <html lang="en">
      <head>
        <meta charSet="utf-8" />
        <meta name="viewport" content="width=device-width, initial-scale=1" />
        <title>{`${title} | ${name}`}</title>
        <meta name="description" content={description} />
        <link rel="stylesheet" href="/assets/shop.css" />
      </head>
      <body>{children}</body>
    </html>
  );
}

function Header({ cartCount }: { cartCount: number }) {
  const { name } = useContext(ShopContext);
  return (
    <header className="header">
      <a className="header__logo" href="/">
        {name}
      </a>
      <form className="header__search" action="/search" role="search">
        <input type="search" name="q" placeholder="Search gear" aria-label="Search" />
        <button type="submit">Search</button>
      </form>
      <ul className="header__account">
        <li>
          <a href="/account">Account</a>
        </li>
        <li>
          <a href="/cart">
            Cart{cartCount > 0 && <span className="header__cart-count">{cartCount}</span>}
          </a>
        </li>
      </ul>
    </header>
  );
}

function NavBar({ categories, active }: { categories: Category[]; active: string }) {
  return (
    <nav className="nav" aria-label="Categories">
      <ul>
        {categories.map(category => (
          <li key={category.slug} className={category.slug === active ? "nav__item nav__item--active" : "nav__item"}>
            <a href={`/c/${category.slug}`} aria-current={category.slug === active ? "page" : undefined}>
              {category.name}
            </a>
          </li>
        ))}
      </ul>
    </nav>
  );
}

function Breadcrumbs({ trail }: { trail: { href: string; label: string }[] }) {
  return (
    <ol className="breadcrumbs">
      {trail.map((step, i) => (
        <li key={step.href}>{i === trail.length - 1 ? <span aria-current="page">{step.label}</span> : <a href={step.href}>{step.label}</a>}</li>
      ))}
    </ol>
  );
}

function Toolbar({ count, onSale }: { count: number; onSale: number }) {
  return (
    <div className="toolbar">
      <p>
        {count} products{onSale > 0 && `, ${onSale} on sale`}
      </p>
      <select name="sort" defaultValue="featured" aria-label="Sort by">
        <option value="featured">Featured</option>
        <option value="price-asc">Price, low to high</option>
        <option value="price-desc">Price, high to low</option>
        <option value="rating">Rating</option>
      </select>
    </div>
  );
}

function Rating({ value, reviews }: { value: number; reviews: number }) {
  const stars: string[] = [];
  for (let star = 1; star <= 5; ++star) stars.push(value >= star ? "full" : value >= star - 0.5 ? "half" : "empty");
  return (
    <div className="rating" aria-label={`${value.toFixed(1)} out of 5`}>
      <ul className="rating__stars">
        {stars.map((kind, i) => (
          <li key={i} className={`star star--${kind}`} />
        ))}
      </ul>
      <span className="rating__count">{reviews === 0 ? "No reviews yet" : `${reviews} reviews`}</span>
    </div>
  );
}

function Price({ priceCents, saleCents }: { priceCents: number; saleCents: number | null }) {
  const { currency } = useContext(ShopContext);
  if (saleCents === null) return <p className="price">{money(currency, priceCents)}</p>;
  return (
    <p className="price price--sale">
      <ins>{money(currency, saleCents)}</ins> <del>{money(currency, priceCents)}</del>{" "}
      <span className="price__off">-{Math.round((1 - saleCents / priceCents) * 100)}%</span>
    </p>
  );
}

function Badges({ badges }: { badges: string[] }) {
  return (
    <ul className="badges">
      {badges.map(badge => (
        <li key={badge} className={`badge badge--${badge}`}>
          {badgeLabels[badge]}
        </li>
      ))}
    </ul>
  );
}

function Swatches({ colors }: { colors: string[] }) {
  return (
    <ul className="swatches" aria-label={`${colors.length} colors`}>
      {colors.map(color => (
        <li key={color}>
          <span className="swatch" style={{ backgroundColor: color }} title={color} />
        </li>
      ))}
    </ul>
  );
}

function Availability({ stock, freeShipping }: { stock: number; freeShipping: boolean }) {
  if (stock === 0) return <p className="stock stock--out">Out of stock</p>;
  return (
    <p className={stock < 5 ? "stock stock--low" : "stock"}>
      {stock < 5 ? `Only ${stock} left` : "In stock"}
      {freeShipping && <span className="stock__shipping">Free shipping</span>}
    </p>
  );
}

function ProductCard({ product }: { product: Product }) {
  const href = `/p/${product.id}`;
  return (
    <article className={product.stock === 0 ? "card card--sold-out" : "card"} data-product={product.id}>
      <a className="card__media" href={href}>
        <img src={`/images/${product.id}.webp`} alt={product.name} width={320} height={320} loading="lazy" />
        {product.badges.length > 0 && <Badges badges={product.badges} />}
      </a>
      <div className="card__body">
        <p className="card__brand">{product.brand}</p>
        <h2 className="card__title">
          <a href={href}>{product.name}</a>
        </h2>
        <Rating value={product.rating} reviews={product.reviews} />
        <Price priceCents={product.priceCents} saleCents={product.saleCents} />
        {product.colors.length > 1 ? <Swatches colors={product.colors} /> : null}
        <Availability stock={product.stock} freeShipping={product.freeShipping} />
        {product.stock > 0 ? (
          <button type="button" className="button" data-add={product.id}>
            Add to cart
          </button>
        ) : (
          <button type="button" className="button button--ghost" disabled>
            Notify me
          </button>
        )}
      </div>
    </article>
  );
}

function Pagination({ slug, current, pages }: { slug: string; current: number; pages: number }) {
  return (
    <nav className="pagination" aria-label="Pages">
      <ul>
        {Array.from({ length: pages }, (_, i) => i + 1).map(page => (
          <li key={page}>{page === current ? <span aria-current="page">{page}</span> : <a href={`/c/${slug}?page=${page}`}>{page}</a>}</li>
        ))}
      </ul>
    </nav>
  );
}

function Footer() {
  const { name } = useContext(ShopContext);
  const columns = [
    { title: "Help", links: ["Shipping", "Returns", "Warranty", "Contact"] },
    { title: "Company", links: ["About", "Stores", "Careers", "Press"] },
    { title: "Community", links: ["Trail reports", "Repairs", "Events"] },
  ];
  return (
    <footer className="footer">
      {columns.map(column => (
        <section key={column.title}>
          <h3>{column.title}</h3>
          <ul>
            {column.links.map(link => (
              <li key={link}>
                <a href={`/${link.toLowerCase().replaceAll(" ", "-")}`}>{link}</a>
              </li>
            ))}
          </ul>
        </section>
      ))}
      <p className="footer__legal">© 2026 {name}</p>
    </footer>
  );
}

function CategoryPage({ category, categories }: { category: Category; categories: Category[] }) {
  const onSale = category.products.filter(product => product.saleCents !== null).length;
  return (
    <Layout title={category.name} description={`${category.products.length} ${category.name.toLowerCase()} in stock and on order.`}>
      <Header cartCount={category.slug.length % 4} />
      <NavBar categories={categories} active={category.slug} />
      <main className="main">
        <Breadcrumbs
          trail={[
            { href: "/", label: "Home" },
            { href: `/c/${category.slug}`, label: category.name },
          ]}
        />
        <h1>{category.name}</h1>
        <Toolbar count={category.products.length} onSale={onSale} />
        <section className="grid">
          {category.products.map(product => (
            <ProductCard key={product.id} product={product} />
          ))}
        </section>
        <Pagination slug={category.slug} current={1} pages={4} />
      </main>
      <Footer />
    </Layout>
  );
}

export async function setup(): Promise<Workload> {
  const categories = catalog();
  return {
    pass() {
      return categories.map(category => {
        const html = renderToString(
          <ShopContext value={shop}>
            <CategoryPage category={category} categories={categories} />
          </ShopContext>,
        );
        if (!html.endsWith("</html>")) throw new Error(`the page of ${category.slug} does not end its document`);
        return `<!DOCTYPE html>${html}`;
      });
    },
  };
}
