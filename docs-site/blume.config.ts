import { defineConfig } from "blume";

const page = (label: string, href: string) => ({ label, href });

export default defineConfig({
  title: "Saccade Documentation",
  description: "Architecture, integration boundaries, and development guidance for Saccade.",
  deployment: {
    site: "https://saccade.cosminbararu.com",
    base: "/docs",
  },
  content: {
    root: "docs",
  },
  feedback: false,
  logo: {
    image: {
      alt: "Saccade",
      dark: "/icon.png",
      light: "/icon.png",
    },
    text: "Saccade",
  },
  navigation: {
    repo: "https://github.com/Cosmin-B/saccade",
    sidebar: {
      display: "group",
      items: [
        { label: "Start here", items: [
          page("Overview", "/"),
          page("Product contract 0.1", "/product/version-0.1"),
        ] },
        { label: "Architecture", items: [
          page("Architecture overview", "/architecture/overview"),
          page("C ABI", "/architecture/abi"),
          page("Agent protocol", "/architecture/agent-protocol"),
          page("Backends", "/architecture/backends"),
          page("Concurrency", "/architecture/concurrency"),
          page("Coordinates", "/architecture/coordinates"),
          page("Fusion", "/architecture/fusion"),
          page("Inference", "/architecture/inference"),
          page("Interaction", "/architecture/interaction"),
          page("Memory", "/architecture/memory"),
          page("Overlay", "/architecture/overlay"),
        ] },
        { label: "Development", items: [
          page("Building", "/development/building"),
          page("Writing", "/development/writing"),
        ] },
      ],
    },
  },
  theme: {
    mode: "dark",
    accent: { dark: "#66d6bd", light: "#276f61" },
    background: { dark: "#0b0d0f", light: "#f1eee7" },
    radius: "sm",
  },
});
