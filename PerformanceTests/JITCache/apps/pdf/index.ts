import { extractText } from "unpdf";

export default {
  async fetch(request: Request) {
    const { totalPages, text } = await extractText(new Uint8Array(await request.arrayBuffer()), { mergePages: true });
    return Response.json({ pages: totalPages, text });
  },
};
