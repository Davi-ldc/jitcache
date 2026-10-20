// Fails to link: two star exports of its source give the binding.
import { maShared } from "./ma-err-stars.mjs";

export const maAmbiguous = maShared;
